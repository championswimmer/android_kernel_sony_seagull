/*
* Copyright(C) 2011-2013 Foxconn International Holdings, Ltd. All rights reserved.
*/


#include <linux/capability.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/elf.h>
#include <linux/elfcore.h>

#include <asm/cacheflush.h>

#ifdef CONFIG_CACHE_L2X0
#include <asm/hardware/cache-l2x0.h>
#endif

#define CRASH_NOTE_NAME "CORE"

#define CRASH_NOTE_MAGIC1 0xCAFEBABE
#define CRASH_NOTE_MAGIC2 0xC001BABE

#define CRASH_NOTE_VM_FLAG_SPARSEMEM	0x01	/* If not set, flatmem is assumed */
#define CRASH_NOTE_VM_FLAG_13SPLIT	0x02	/* 1GB/3GB user/kernel split */
#define CRASH_NOTE_VM_FLAG_22SPLIT	0x04	/* 2GB/2GB user/kernel split */
#define CRASH_NOTE_VM_FLAG_31SPLIT	0x08	/* 3GB/1GB user/kernel split */

#define CRASH_NOTE_MAGIC_FLAG_VM		0x01 /* Has VM information */
#define CRASH_NOTE_MAGIC_FLAG_REGISTERS		0x02 /* Registers are reliable */

#ifdef CONFIG_CRASH_NOTES_MAGIC
#ifdef CONFIG_CRASH_NOTES_VM
#define CRASH_NOTE_MAGIC_BYTES  (8 * ALIGN(sizeof(u32), 4))
#else
#define CRASH_NOTE_MAGIC_BYTES (6 * ALIGN(sizeof(u32), 4))
#endif
#else
#define CRASH_NOTE_MAGIC_BYTES 0
#endif

#define CRASH_NOTE_SIZE (ALIGN(sizeof(struct elf_note), 4) + \
			 ALIGN(sizeof(CRASH_NOTE_NAME), 4) + \
			 ALIGN(sizeof(struct elf_prstatus), 4))

#define CRASH_NOTE_BYTES (2 * CRASH_NOTE_SIZE + CRASH_NOTE_MAGIC_BYTES)

#ifdef CONFIG_CRASH_NOTES_MAGIC
struct crash_extras {
	u32 magic_part1;
	u32 magic_part2;
	u32 magic_note_size; /* Size of the note field */
	u32 magic_total_size; /* Size of the crash_notes buffer (including extras) */
	u32 magic_flags;
#ifdef CONFIG_CRASH_NOTES_VM
	u32 vm_start;
	u32 vm_flags;
#endif
	u32 checksum; /* Simple XOR of this block minus this entry */
};
#endif

typedef u32 note_buf_t[CRASH_NOTE_BYTES/4];

note_buf_t* crash_notes;

static inline void dump_regs(struct pt_regs *regs)
{
	/* dump critical general registers first */
	__asm__ __volatile__("str fp, %0" : "=m"(regs->ARM_fp));
	__asm__ __volatile__("str sp, %0" : "=m"(regs->ARM_sp));
	__asm__ __volatile__("str pc, %0" : "=m"(regs->ARM_pc));
	__asm__ __volatile__("str lr, %0" : "=m"(regs->ARM_lr));
	/* dump general registers that will be used later */
	__asm__ __volatile__("str r0, %0" : "=m"(regs->ARM_r0));
	__asm__ __volatile__("str r1, %0" : "=m"(regs->ARM_r1));
	__asm__ __volatile__("str r2, %0" : "=m"(regs->ARM_r2));
	__asm__ __volatile__("str r3, %0" : "=m"(regs->ARM_r3));
	__asm__ __volatile__("str r4, %0" : "=m"(regs->ARM_r4));
	__asm__ __volatile__("str r5, %0" : "=m"(regs->ARM_r5));
	__asm__ __volatile__("str r6, %0" : "=m"(regs->ARM_r6));
	__asm__ __volatile__("str r7, %0" : "=m"(regs->ARM_r7));
	__asm__ __volatile__("str r8, %0" : "=m"(regs->ARM_r8));
	__asm__ __volatile__("str r9, %0" : "=m"(regs->ARM_r9));
	__asm__ __volatile__("str r10, %0": "=m"(regs->ARM_r10));
	__asm__ __volatile__("str ip, %0" : "=m"(regs->ARM_ip));
	/* The registers involved with processor states and cp states
	 * will not be changed in the above operation, so it is safe
	 * to dump them at last
	 */
	/* dump cpsr register */
	__asm__ __volatile__("mrs %0, cpsr" : "=r"(regs->ARM_cpsr));
}

static void write_crash_notes(int real)
{
	struct elf_prstatus prstatus;
	struct elf_note *note;
	u32 *buf;
	u32 *start;
	char process_name[TASK_COMM_LEN];
#ifdef CONFIG_CRASH_NOTES_MAGIC
	u32 c;
	struct crash_extras* extras;
#endif

	buf = (u32*)per_cpu_ptr(crash_notes, 0);
	if (!buf)
		return;

	start = buf;
	memset(&prstatus, 0, sizeof(prstatus));
	prstatus.pr_pid = current->pid;

	BUG_ON(sizeof(prstatus.pr_reg) != sizeof(struct pt_regs));
	if (real) {
		dump_regs((struct pt_regs*)&prstatus.pr_reg);
		get_task_comm(process_name, current);
		printk(KERN_ERR "&@panic_name@:*%s*\n", process_name);
	}
	note = (struct elf_note*)buf;
	note->n_namesz = strlen(CRASH_NOTE_NAME) + 1;
	note->n_descsz = sizeof(prstatus);
	note->n_type   = NT_PRSTATUS;
	buf += (sizeof(struct elf_note) + 3) / 4;
	memcpy(buf, CRASH_NOTE_NAME, note->n_namesz);
	buf += (note->n_namesz + 3) / 4;
	memcpy(buf, &prstatus, sizeof(prstatus));
	buf += (note->n_descsz + 3) / 4;

	note = (struct elf_note*)buf;
	note->n_namesz = 0;
	note->n_descsz = 0;
	note->n_type   = 0;

#ifdef CONFIG_CRASH_NOTES_MAGIC
	extras = (struct crash_extras*)( ((u8*)start) + CRASH_NOTE_BYTES - CRASH_NOTE_MAGIC_BYTES);
	extras->magic_note_size = CRASH_NOTE_SIZE;
	extras->magic_total_size = CRASH_NOTE_BYTES;
	extras->magic_part1 = CRASH_NOTE_MAGIC1;
	extras->magic_part2 = CRASH_NOTE_MAGIC2;
	extras->magic_flags = 0;
	if (real)
		extras->magic_flags |= CRASH_NOTE_MAGIC_FLAG_REGISTERS;

#ifdef CONFIG_CRASH_NOTES_VM
	extras->magic_flags |= CRASH_NOTE_MAGIC_FLAG_VM;
	extras->vm_flags = 0;
	extras->vm_start = CONFIG_PAGE_OFFSET;
#ifdef CONFIG_SPARSEMEM
	extras->vm_flags |= CRASH_NOTE_VM_FLAG_SPARSEMEM;
#endif
#ifdef CONFIG_VMSPLIT_1G
	extras->vm_flags |= CRASH_NOTE_VM_FLAG_13SPLIT;
#endif
#ifdef CONFIG_VMSPLIT_2G
	extras->vm_flags |= CRASH_NOTE_VM_FLAG_22SPLIT;
#endif
#ifdef CONFIG_VMSPLIT_3G
	extras->vm_flags |= CRASH_NOTE_VM_FLAG_31SPLIT;
#endif
#endif

	/* Calculate an XOR checksum of all data in the elf_note_extra */
	extras->checksum = 0;
	for (c = 0; c != (CRASH_NOTE_BYTES - sizeof(u32))/sizeof(u32); ++c)
	{
		extras->checksum ^= start[c];
	}

#endif

	if (real) {
		/* Make sure we have crash_notes in ram before reset */
		flush_cache_all();
		#ifdef CONFIG_CACHE_L2X0
		//l2x0_suspend();
		#endif
	}

}

static int update_crash_notes(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	write_crash_notes(1);

	return NOTIFY_DONE;
}

static struct notifier_block panic_block = {
	.notifier_call = update_crash_notes,
};

static int __init crash_notes_init(void)
{
	/* Allocate memory for saving cpu registers. */
	crash_notes = alloc_percpu(note_buf_t);
	if (!crash_notes) {
		printk("crash: Memory allocation for saving cpu register"
		       " states failed\n");
		return -ENOMEM;
	}

	/* Initialize memory with something that the tools pick up on */
	/* It will NOT be useful register info, but it's something at least */
	write_crash_notes(0);

	atomic_notifier_chain_register(&panic_notifier_list, &panic_block);

	return 0;
}
module_init(crash_notes_init)

