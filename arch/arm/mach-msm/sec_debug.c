/*
 * sec_debug.c
 *
 * driver supporting debug functions for Samsung device
 *
 * COPYRIGHT(C) Samsung Electronics Co., Ltd. 2006-2011 All Right Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/ctype.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/sysrq.h>
#include <asm/cacheflush.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/sec_param.h>
#include <mach/system.h>
#include <mach/sec_debug.h>
#include <mach/msm_iomap.h>
#include <mach/msm_smsm.h>
#ifdef CONFIG_SEC_DEBUG_LOW_LOG
#include <linux/seq_file.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#endif
#include <linux/debugfs.h>
#include <asm/system_info.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mount.h>
#include <linux/utsname.h>
#include <linux/seq_file.h>
#include <linux/nmi.h>

#ifdef CONFIG_HOTPLUG_CPU
#include <linux/cpumask.h>
#include <linux/cpu.h>
#include <linux/irq.h>
#include <linux/preempt.h>
#endif
#ifdef CONFIG_SEC_DEBUG_SEC_WDOG_BITE
#include <mach/scm.h>
#endif

#if defined(CONFIG_ARCH_MSM8974) || defined(CONFIG_ARCH_MSM8226)
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#endif
#if defined (CONFIG_MACH_AFYONLTE_TMO) || defined(CONFIG_MACH_ATLANTICLTE_ATT) || defined(CONFIG_MACH_ATLANTIC3GEUR_OPEN)
#include <asm/hardware/gic.h>
#endif
#include <linux/vmalloc.h>

#ifdef CONFIG_SEC_DEBUG_VERBOSE_SUMMARY_HTML
unsigned int cpu_frequency[CONFIG_NR_CPUS];
unsigned int cpu_volt[CONFIG_NR_CPUS];
char cpu_state[CONFIG_NR_CPUS][VAR_NAME_MAX];
EXPORT_SYMBOL(cpu_frequency);
EXPORT_SYMBOL(cpu_volt);
EXPORT_SYMBOL(cpu_state);
#endif

/* onlyjazz.ed26 : make the restart_reason global to enable it early
   in sec_debug_init and share with restart functions */

#ifdef CONFIG_SEC_DEBUG_DOUBLE_FREE
#include <linux/circ_buf.h>
#endif

#include <linux/init.h>

#ifdef CONFIG_USER_RESET_DEBUG
enum sec_debug_reset_reason_t {
	RR_S = 1,
	RR_W = 2,
	RR_D = 3,
	RR_K = 4,
	RR_M = 5,
	RR_P = 6,
	RR_R = 7,
	RR_B = 8,
	RR_N = 9,
};
#endif

enum sec_debug_upload_cause_t {
	UPLOAD_CAUSE_INIT = 0xCAFEBABE,
	UPLOAD_CAUSE_KERNEL_PANIC = 0x000000C8,
	UPLOAD_CAUSE_POWER_LONG_PRESS = 0x00000085,
	UPLOAD_CAUSE_FORCED_UPLOAD = 0x00000022,
	UPLOAD_CAUSE_CP_ERROR_FATAL = 0x000000CC,
	UPLOAD_CAUSE_MDM_ERROR_FATAL = 0x000000EE,
	UPLOAD_CAUSE_USER_FAULT = 0x0000002F,
	UPLOAD_CAUSE_HSIC_DISCONNECTED = 0x000000DD,
	UPLOAD_CAUSE_MODEM_RST_ERR = 0x000000FC,
	UPLOAD_CAUSE_RIVA_RST_ERR = 0x000000FB,
	UPLOAD_CAUSE_LPASS_RST_ERR = 0x000000FA,
	UPLOAD_CAUSE_DSPS_RST_ERR = 0x000000FD,
	UPLOAD_CAUSE_PERIPHERAL_ERR = 0x000000FF,
	UPLOAD_CAUSE_NON_SECURE_WDOG_BARK = 0x00000DBA,
	UPLOAD_CAUSE_NON_SECURE_WDOG_BITE = 0x00000DBE,
	UPLOAD_CAUSE_SECURE_WDOG_BITE = 0x00005DBE,
	UPLOAD_CAUSE_BUS_HANG = 0x000000B5,
};

struct sec_debug_mmu_reg_t {
	int SCTLR;
	int TTBR0;
	int TTBR1;
	int TTBCR;
	int DACR;
	int DFSR;
	int DFAR;
	int IFSR;
	int IFAR;
	int DAFSR;
	int IAFSR;
	int PMRRR;
	int NMRRR;
	int FCSEPID;
	int CONTEXT;
	int URWTPID;
	int UROTPID;
	int POTPIDR;
};

/* ARM CORE regs mapping structure */
struct sec_debug_core_t {
	/* COMMON */
	unsigned int r0;
	unsigned int r1;
	unsigned int r2;
	unsigned int r3;
	unsigned int r4;
	unsigned int r5;
	unsigned int r6;
	unsigned int r7;
	unsigned int r8;
	unsigned int r9;
	unsigned int r10;
	unsigned int r11;
	unsigned int r12;

	/* SVC */
	unsigned int r13_svc;
	unsigned int r14_svc;
	unsigned int spsr_svc;

	/* PC & CPSR */
	unsigned int pc;
	unsigned int cpsr;

	/* USR/SYS */
	unsigned int r13_usr;
	unsigned int r14_usr;

	/* FIQ */
	unsigned int r8_fiq;
	unsigned int r9_fiq;
	unsigned int r10_fiq;
	unsigned int r11_fiq;
	unsigned int r12_fiq;
	unsigned int r13_fiq;
	unsigned int r14_fiq;
	unsigned int spsr_fiq;

	/* IRQ */
	unsigned int r13_irq;
	unsigned int r14_irq;
	unsigned int spsr_irq;

	/* MON */
	unsigned int r13_mon;
	unsigned int r14_mon;
	unsigned int spsr_mon;

	/* ABT */
	unsigned int r13_abt;
	unsigned int r14_abt;
	unsigned int spsr_abt;

	/* UNDEF */
	unsigned int r13_und;
	unsigned int r14_und;
	unsigned int spsr_und;
};

/* enable sec_debug feature */
static unsigned enable = 0;
static unsigned enable_user = 0;
static unsigned reset_reason = 0xFFEEFFEE;
static char sec_build_info[100];
static unsigned int secdbg_paddr;
static unsigned int secdbg_size;
static unsigned pm8941_rev = 0;
static unsigned pm8841_rev = 0;
unsigned int sec_dbg_level = 0;

uint runtime_debug_val;
static uint32_t tzapps_start_addr;
static uint32_t tzapps_size;

module_param_named(enable, enable, uint, 0644);
module_param_named(enable_user, enable_user, uint, 0644);
module_param_named(reset_reason, reset_reason, uint, 0644);
module_param_named(runtime_debug_val, runtime_debug_val, uint, 0644);
#ifdef CONFIG_ARCH_MSM8974PRO
module_param_named(pmc8974_rev, pmc8974_rev, uint, 0644);
#else
module_param_named(pm8941_rev, pm8941_rev, uint, 0644);
module_param_named(pm8841_rev, pm8841_rev, uint, 0644);
#endif

static int sec_alloc_virtual_mem(const char *val, struct kernel_param *kp);
module_param_call(alloc_virtual_mem, sec_alloc_virtual_mem, NULL, NULL, 0644);

static int sec_free_virtual_mem(const char *val, struct kernel_param *kp);
module_param_call(free_virtual_mem, sec_free_virtual_mem, NULL, NULL, 0644);

static int sec_alloc_physical_mem(const char *val, struct kernel_param *kp);
module_param_call(alloc_physical_mem, sec_alloc_physical_mem, NULL, NULL, 0644);

static int sec_free_physical_mem(const char *val, struct kernel_param *kp);
module_param_call(free_physical_mem, sec_free_physical_mem, NULL, NULL, 0644);

static int dbg_set_cpu_affinity(const char *val, struct kernel_param *kp);
module_param_call(setcpuaff, dbg_set_cpu_affinity, NULL, NULL, 0644);
static char *sec_build_time[] = {
	__DATE__,
	__TIME__
};
static char build_root[] = __FILE__;

/* klaatu - schedule log */
#ifdef CONFIG_SEC_DEBUG_SCHED_LOG
struct sec_debug_log {
	atomic_t idx_sched[CONFIG_NR_CPUS];
	struct sched_log sched[CONFIG_NR_CPUS][SCHED_LOG_MAX];

	atomic_t idx_irq[CONFIG_NR_CPUS];
	struct irq_log irq[CONFIG_NR_CPUS][SCHED_LOG_MAX];

	atomic_t idx_secure[CONFIG_NR_CPUS];
	struct secure_log secure[CONFIG_NR_CPUS][TZ_LOG_MAX];

	atomic_t idx_irq_exit[CONFIG_NR_CPUS];
	struct irq_exit_log irq_exit[CONFIG_NR_CPUS][SCHED_LOG_MAX];

	atomic_t idx_timer[CONFIG_NR_CPUS];
	struct timer_log timer_log[CONFIG_NR_CPUS][SCHED_LOG_MAX];

#ifdef CONFIG_SEC_DEBUG_MSG_LOG
	atomic_t idx_secmsg[CONFIG_NR_CPUS];
	struct secmsg_log secmsg[CONFIG_NR_CPUS][MSG_LOG_MAX];
#endif
#ifdef CONFIG_SEC_DEBUG_AVC_LOG
	atomic_t idx_secavc[CONFIG_NR_CPUS];
	struct secavc_log secavc[CONFIG_NR_CPUS][AVC_LOG_MAX];
#endif
#ifdef CONFIG_SEC_DEBUG_DCVS_LOG
	atomic_t dcvs_log_idx[CONFIG_NR_CPUS] ;
	struct dcvs_debug dcvs_log[CONFIG_NR_CPUS][DCVS_LOG_MAX] ;
#endif
#ifdef CONFIG_SEC_DEBUG_FUELGAUGE_LOG
	atomic_t fg_log_idx;
	struct fuelgauge_debug fg_log[FG_LOG_MAX] ;
#endif
	/* zwei variables -- last_pet und last_ns */
	unsigned long long last_pet;
	atomic64_t last_ns;
};

struct sec_debug_log *secdbg_log;
struct sec_debug_subsys *secdbg_subsys;
struct sec_debug_subsys_data_krait *secdbg_krait;

#endif	/* CONFIG_SEC_DEBUG_SCHED_LOG */

#define MEM_ADDRESS_FILE_NAME "mem.address"

static struct proc_dir_entry *sec_debug_dir_de = NULL;
static struct proc_dir_entry *mem_access_dir_de = NULL;
static struct proc_dir_entry *mem_access_file_de = NULL;

extern void pde_put(struct proc_dir_entry *pde);
extern struct inode *proc_get_inode(struct super_block *sb, struct proc_dir_entry *de);

static int sec_debug_mem_write(struct file *file, const char __user *buffer,
                size_t count, loff_t *offs);

static int sec_debug_mem_read(struct file *file, char __user *buffer,
                size_t count, loff_t *offs);

static int proc_delete_dentry(const struct dentry * dentry)
{
        return 1;
}

static const struct dentry_operations proc_dentry_operations =
{
        .d_delete       = proc_delete_dentry,
};

static const struct file_operations sec_debug_mem_access_proc_fops = {
        .write = sec_debug_mem_write,
        .read = sec_debug_mem_read,
};

static struct inode_operations mem_access_proc_dir_inode_operations;

/* onlyjazz.ed26 : make the restart_reason global to enable it early
   in sec_debug_init and share with restart functions */
void *restart_reason;
#ifdef CONFIG_RESTART_REASON_DDR
void *restart_reason_ddr_address = NULL;
/* Using bottom of sec_dbg DDR address range for writting restart reason */
#ifdef CONFIG_SEC_LPDDR_6G
#define  RESTART_REASON_DDR_ADDR 0x2FFFE000
#else
#define  RESTART_REASON_DDR_ADDR 0x3FFFE000
#endif
#endif

DEFINE_PER_CPU(struct sec_debug_core_t, sec_debug_core_reg);
DEFINE_PER_CPU(struct sec_debug_mmu_reg_t, sec_debug_mmu_reg);
DEFINE_PER_CPU(enum sec_debug_upload_cause_t, sec_debug_upload_cause);

/* save last_pet and last_ns with these nice functions */
void sec_debug_save_last_pet(unsigned long long last_pet)
{
	if(secdbg_log)
		secdbg_log->last_pet = last_pet;
}

void sec_debug_save_last_ns(unsigned long long last_ns)
{
	if(secdbg_log)
		atomic64_set(&(secdbg_log->last_ns), last_ns);
}
EXPORT_SYMBOL(sec_debug_save_last_pet);
EXPORT_SYMBOL(sec_debug_save_last_ns);

#ifdef CONFIG_HOTPLUG_CPU
static void pull_down_other_cpus(void)
{
	int cpu;
	for_each_online_cpu(cpu) {
		if(cpu == 0)
			continue;
		cpu_down(cpu);
	}
}
#else
static void pull_down_other_cpus(void)
{
}
#endif

static int * g_allocated_phys_mem = NULL;
static int * g_allocated_virt_mem = NULL;

static int sec_alloc_virtual_mem(const char *val, struct kernel_param *kp)
{
	int * mem;
	char * str = (char *) val;
	unsigned size = (unsigned) memparse(str, &str);
	if(size)
	{
		mem = vmalloc(size);
		if (mem) {
			*mem = (int) g_allocated_virt_mem;
			g_allocated_virt_mem = mem;
			return 0;
		}
	}

	return -EAGAIN;
}

static int sec_free_virtual_mem(const char *val, struct kernel_param *kp)
{
	int * mem;
	char * str = (char *) val;
        unsigned free_count = (unsigned) memparse(str, &str);

	if(!free_count)
	{
		if(strncmp(val, "all", 4))
		{
			free_count = 10;
		}
		else
		{
			return -EAGAIN;
		}
	}

	if(free_count>10)
		free_count = 10;

        if(!g_allocated_virt_mem)
        {
                return 0;
        }

	while(g_allocated_virt_mem && free_count--)
	{
		mem = (int *) *g_allocated_virt_mem;
		vfree(g_allocated_virt_mem);
		g_allocated_virt_mem = mem;
	}


	return 0;
}

static int sec_alloc_physical_mem(const char *val, struct kernel_param *kp)
{
        int * mem;
	char * str = (char *) val;
        unsigned size = (unsigned) memparse(str, &str);
        if(size)
        {
                mem = kmalloc(size, GFP_KERNEL);
                if(mem)
                {
                        *mem = (int) g_allocated_phys_mem;
                        g_allocated_phys_mem = mem;
			return 0;
                }
        }

        return -EAGAIN;
}

static int sec_free_physical_mem(const char *val, struct kernel_param *kp)
{
        int * mem;
        char * str = (char *) val;
        unsigned free_count = (unsigned) memparse(str, &str);

        if(!free_count)
        {
                if(strncmp(val, "all", 4))
                {
                        free_count = 10;
                }
                else
                {
                        return -EAGAIN;
                }
        }

        if(free_count>10)
                free_count = 10;

	if(!g_allocated_phys_mem)
	{
		return 0;
	}

        while(g_allocated_phys_mem && free_count--)
        {
                mem = (int *) *g_allocated_phys_mem;
                kfree(g_allocated_phys_mem);
                g_allocated_phys_mem = mem;
        }

	return 0;
}

static int dbg_set_cpu_affinity(const char *val, struct kernel_param *kp)
{
	char *endptr;
	pid_t pid;
	int cpu;
	struct cpumask mask;
	long ret;
	pid = (pid_t)memparse(val, &endptr);
	if (*endptr != '@') {
		return -EINVAL;
	}
	cpu = memparse(++endptr, &endptr);
	cpumask_clear(&mask);
	cpumask_set_cpu(cpu, &mask);
	ret = sched_setaffinity(pid, &mask);
	return 0;
}

/* for sec debug level */
static int __init sec_debug_level(char *str)
{
	get_option(&str, &sec_dbg_level);
	return 0;
}
early_param("level", sec_debug_level);

bool kernel_sec_set_debug_level(int level)
{
	if (!(level == KERNEL_SEC_DEBUG_LEVEL_LOW
			|| level == KERNEL_SEC_DEBUG_LEVEL_MID
			|| level == KERNEL_SEC_DEBUG_LEVEL_HIGH)) {
		pr_notice(KERN_NOTICE "(kernel_sec_set_debug_level) The debug"\
				"value is invalid(0x%x)!! Set default"\
				"level(LOW)\n", level);
		sec_dbg_level = KERNEL_SEC_DEBUG_LEVEL_LOW;
		return -EINVAL;
	}

	sec_dbg_level = level;

	switch (level) {
	case KERNEL_SEC_DEBUG_LEVEL_LOW:
		enable = 0;
		enable_user = 0;
		break;
	case KERNEL_SEC_DEBUG_LEVEL_MID:
		enable = 1;
		enable_user = 0;
		break;
	case KERNEL_SEC_DEBUG_LEVEL_HIGH:
		enable = 1;
		enable_user = 1;
		break;
	default:
		enable = 1;
		enable_user = 1;
	}

	/* write to param */
	sec_set_param(param_index_debuglevel, &sec_dbg_level);

	pr_notice(KERN_NOTICE "(kernel_sec_set_debug_level)"\
			"The debug value is 0x%x !!\n", level);

	return 1;
}
EXPORT_SYMBOL(kernel_sec_set_debug_level);

int kernel_sec_get_debug_level(void)
{
	sec_get_param(param_index_debuglevel, &sec_dbg_level);

	if (!(sec_dbg_level == KERNEL_SEC_DEBUG_LEVEL_LOW
			|| sec_dbg_level == KERNEL_SEC_DEBUG_LEVEL_MID
			|| sec_dbg_level == KERNEL_SEC_DEBUG_LEVEL_HIGH)) {
		/*In case of invalid debug level, default (debug level low)*/
		pr_notice(KERN_NOTICE "(%s) The debug value is"\
				"invalid(0x%x)!! Set default level(LOW)\n",
				__func__, sec_dbg_level);
		sec_dbg_level = KERNEL_SEC_DEBUG_LEVEL_LOW;
		sec_set_param(param_index_debuglevel, &sec_dbg_level);
	}
	return sec_dbg_level;
}
EXPORT_SYMBOL(kernel_sec_get_debug_level);

#ifdef CONFIG_SEC_MONITOR_BATTERY_REMOVAL
static unsigned normal_off = 0;
static int __init power_normal_off(char *val)
{
	normal_off = strncmp(val, "1", 1) ? 0 : 1;
        return 1;
}
__setup("normal_off=", power_normal_off);

bool kernel_sec_set_normal_pwroff(int value)
{
	int normal_poweroff = value;
	sec_set_param(param_index_normal_poweroff, &normal_poweroff);

	return 1;
}
EXPORT_SYMBOL(kernel_sec_set_normal_pwroff);

static int sec_get_normal_off(void *data, u64 *val)
{
        *val = normal_off;
        return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(normal_off_fops, sec_get_normal_off, NULL, "%lld\n");

static int __init sec_logger_init(void)
{
#ifdef CONFIG_DEBUG_FS
        struct dentry *dent;
        struct dentry   *dbgfs_file;

        dent = debugfs_create_dir("sec_logger", 0);
        if (IS_ERR_OR_NULL(dent)) {
                pr_err("Failed to create debugfs dir of sec_logger\n");
                return PTR_ERR(dent);
	}

        dbgfs_file = debugfs_create_file("normal_off", 0644, dent,
						 NULL, &normal_off_fops);
        if (IS_ERR_OR_NULL(dbgfs_file)) {
                pr_err("Failed to create debugfs file of normal_off file\n");
	        debugfs_remove_recursive(dent);
                return PTR_ERR(dbgfs_file);
        }
#endif
        return 0;
}
late_initcall(sec_logger_init);
#endif

/* core reg dump function*/
static void sec_debug_save_core_reg(struct sec_debug_core_t *core_reg)
{
	/* we will be in SVC mode when we enter this function. Collect
	   SVC registers along with cmn registers. */
	asm("str r0, [%0,#0]\n\t"	/* R0 is pushed first to core_reg */
	    "mov r0, %0\n\t"		/* R0 will be alias for core_reg */
	    "str r1, [r0,#4]\n\t"	/* R1 */
	    "str r2, [r0,#8]\n\t"	/* R2 */
	    "str r3, [r0,#12]\n\t"	/* R3 */
	    "str r4, [r0,#16]\n\t"	/* R4 */
	    "str r5, [r0,#20]\n\t"	/* R5 */
	    "str r6, [r0,#24]\n\t"	/* R6 */
	    "str r7, [r0,#28]\n\t"	/* R7 */
	    "str r8, [r0,#32]\n\t"	/* R8 */
	    "str r9, [r0,#36]\n\t"	/* R9 */
	    "str r10, [r0,#40]\n\t"	/* R10 */
	    "str r11, [r0,#44]\n\t"	/* R11 */
	    "str r12, [r0,#48]\n\t"	/* R12 */
	    /* SVC */
	    "str r13, [r0,#52]\n\t"	/* R13_SVC */
	    "str r14, [r0,#56]\n\t"	/* R14_SVC */
	    "mrs r1, spsr\n\t"		/* SPSR_SVC */
	    "str r1, [r0,#60]\n\t"
	    /* PC and CPSR */
	    "sub r1, r15, #0x4\n\t"	/* PC */
	    "str r1, [r0,#64]\n\t"
	    "mrs r1, cpsr\n\t"		/* CPSR */
	    "str r1, [r0,#68]\n\t"
	    /* SYS/USR */
	    "mrs r1, cpsr\n\t"		/* switch to SYS mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x1f\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#72]\n\t"	/* R13_USR */
	    "str r14, [r0,#76]\n\t"	/* R14_USR */
	    /* FIQ */
	    "mrs r1, cpsr\n\t"		/* switch to FIQ mode */
	    "and r1,r1,#0xFFFFFFE0\n\t"
	    "orr r1,r1,#0x11\n\t"
	    "msr cpsr,r1\n\t"
	    "str r8, [r0,#80]\n\t"	/* R8_FIQ */
	    "str r9, [r0,#84]\n\t"	/* R9_FIQ */
	    "str r10, [r0,#88]\n\t"	/* R10_FIQ */
	    "str r11, [r0,#92]\n\t"	/* R11_FIQ */
	    "str r12, [r0,#96]\n\t"	/* R12_FIQ */
	    "str r13, [r0,#100]\n\t"	/* R13_FIQ */
	    "str r14, [r0,#104]\n\t"	/* R14_FIQ */
	    "mrs r1, spsr\n\t"		/* SPSR_FIQ */
	    "str r1, [r0,#108]\n\t"
		/* IRQ */
	    "mrs r1, cpsr\n\t"		/* switch to IRQ mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x12\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#112]\n\t"	/* R13_IRQ */
	    "str r14, [r0,#116]\n\t"	/* R14_IRQ */
	    "mrs r1, spsr\n\t"		/* SPSR_IRQ */
	    "str r1, [r0,#120]\n\t"
	    /* MON */
	    "mrs r1, cpsr\n\t"		/* switch to monitor mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x16\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#124]\n\t"	/* R13_MON */
	    "str r14, [r0,#128]\n\t"	/* R14_MON */
	    "mrs r1, spsr\n\t"		/* SPSR_MON */
	    "str r1, [r0,#132]\n\t"
	    /* ABT */
	    "mrs r1, cpsr\n\t"		/* switch to Abort mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x17\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#136]\n\t"	/* R13_ABT */
	    "str r14, [r0,#140]\n\t"	/* R14_ABT */
	    "mrs r1, spsr\n\t"		/* SPSR_ABT */
	    "str r1, [r0,#144]\n\t"
	    /* UND */
	    "mrs r1, cpsr\n\t"		/* switch to undef mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x1B\n\t"
	    "msr cpsr,r1\n\t"
	    "str r13, [r0,#148]\n\t"	/* R13_UND */
	    "str r14, [r0,#152]\n\t"	/* R14_UND */
	    "mrs r1, spsr\n\t"		/* SPSR_UND */
	    "str r1, [r0,#156]\n\t"
	    /* restore to SVC mode */
	    "mrs r1, cpsr\n\t"		/* switch to SVC mode */
	    "and r1, r1, #0xFFFFFFE0\n\t"
	    "orr r1, r1, #0x13\n\t"
	    "msr cpsr,r1\n\t" :		/* output */
	    : "r"(core_reg)			/* input */
	    : "%r0", "%r1"		/* clobbered registers */
	);

	return;
}

static void sec_debug_save_mmu_reg(struct sec_debug_mmu_reg_t *mmu_reg)
{
	asm("mrc    p15, 0, r1, c1, c0, 0\n\t"	/* SCTLR */
	    "str r1, [%0]\n\t"
	    "mrc    p15, 0, r1, c2, c0, 0\n\t"	/* TTBR0 */
	    "str r1, [%0,#4]\n\t"
	    "mrc    p15, 0, r1, c2, c0,1\n\t"	/* TTBR1 */
	    "str r1, [%0,#8]\n\t"
	    "mrc    p15, 0, r1, c2, c0,2\n\t"	/* TTBCR */
	    "str r1, [%0,#12]\n\t"
	    "mrc    p15, 0, r1, c3, c0,0\n\t"	/* DACR */
	    "str r1, [%0,#16]\n\t"
	    "mrc    p15, 0, r1, c5, c0,0\n\t"	/* DFSR */
	    "str r1, [%0,#20]\n\t"
	    "mrc    p15, 0, r1, c6, c0,0\n\t"	/* DFAR */
	    "str r1, [%0,#24]\n\t"
	    "mrc    p15, 0, r1, c5, c0,1\n\t"	/* IFSR */
	    "str r1, [%0,#28]\n\t"
	    "mrc    p15, 0, r1, c6, c0,2\n\t"	/* IFAR */
	    "str r1, [%0,#32]\n\t"
	    /* Don't populate DAFSR and RAFSR */
	    "mrc    p15, 0, r1, c10, c2,0\n\t"	/* PMRRR */
	    "str r1, [%0,#44]\n\t"
	    "mrc    p15, 0, r1, c10, c2,1\n\t"	/* NMRRR */
	    "str r1, [%0,#48]\n\t"
	    "mrc    p15, 0, r1, c13, c0,0\n\t"	/* FCSEPID */
	    "str r1, [%0,#52]\n\t"
	    "mrc    p15, 0, r1, c13, c0,1\n\t"	/* CONTEXT */
	    "str r1, [%0,#56]\n\t"
	    "mrc    p15, 0, r1, c13, c0,2\n\t"	/* URWTPID */
	    "str r1, [%0,#60]\n\t"
	    "mrc    p15, 0, r1, c13, c0,3\n\t"	/* UROTPID */
	    "str r1, [%0,#64]\n\t"
	    "mrc    p15, 0, r1, c13, c0,4\n\t"	/* POTPIDR */
	    "str r1, [%0,#68]\n\t" :		/* output */
	    : "r"(mmu_reg)			/* input */
	    : "%r1", "memory"			/* clobbered register */
	);
}

static void sec_debug_save_context(void)
{
	unsigned long flags;
	local_irq_save(flags);
	sec_debug_save_mmu_reg(&per_cpu
			(sec_debug_mmu_reg, smp_processor_id()));
	sec_debug_save_core_reg(&per_cpu
			(sec_debug_core_reg, smp_processor_id()));
	pr_emerg("(%s) context saved(CPU:%d)\n", __func__,
			smp_processor_id());
	local_irq_restore(flags);
}

extern void set_dload_mode(int on);
static void sec_debug_set_qc_dload_magic(int on)
{
	set_dload_mode(on);
}

#define RESTART_REASON_ADDR 0x65C
static void sec_debug_set_upload_magic(unsigned magic)
{
	pr_emerg("(%s) %x\n", __func__, magic);

	if (magic)
		sec_debug_set_qc_dload_magic(1);

	restart_reason = MSM_IMEM_BASE + RESTART_REASON_ADDR;
	__raw_writel(magic, restart_reason);

	flush_cache_all();
	outer_flush_all();
}

static int sec_debug_normal_reboot_handler(struct notifier_block *nb,
		unsigned long l, void *p)
{
	sec_debug_set_upload_magic(0x0);
	return 0;
}

static void sec_debug_set_upload_cause(enum sec_debug_upload_cause_t type)
{
#ifdef CONFIG_RESTART_REASON_DDR
	void *upload_cause_ddr_address = restart_reason_ddr_address + 0x10;
#endif
	void * upload_cause = MSM_IMEM_BASE + 0x66C;
	per_cpu(sec_debug_upload_cause, smp_processor_id()) = type;
	__raw_writel(type, upload_cause);

#ifdef CONFIG_RESTART_REASON_DDR
	if(restart_reason_ddr_address) {
		if(type == UPLOAD_CAUSE_POWER_LONG_PRESS) {
			/* UPLOAD_CAUSE_POWER_LONG_PRESS magic number to DDR restart reason address */
			__raw_writel(UPLOAD_CAUSE_POWER_LONG_PRESS, upload_cause_ddr_address);
		}
		/* if power key is released after pressing, clear the DDR */
		if(type == UPLOAD_CAUSE_INIT &&
				(UPLOAD_CAUSE_POWER_LONG_PRESS == __raw_readl(upload_cause_ddr_address))) {
			__raw_writel(0x0, upload_cause_ddr_address);
		}
	}
#endif
}

extern struct uts_namespace init_uts_ns;
void sec_debug_hw_reset(void)
{
	pr_emerg("(%s) %s %s\n", __func__, init_uts_ns.name.release,
						init_uts_ns.name.version);
	pr_emerg("(%s) rebooting...\n", __func__);
	flush_cache_all();
	outer_flush_all();
	msm_restart(0, "sec_debug_hw_reset");

	while (1)
		;
}
EXPORT_SYMBOL(sec_debug_hw_reset);

#ifdef CONFIG_SEC_PERIPHERAL_SECURE_CHK
void sec_peripheral_secure_check_fail(void)
{
	sec_debug_set_upload_magic(0x77665507);
	sec_debug_set_qc_dload_magic(0);
        printk("sec_periphe\n");
        pr_emerg("(%s) %s\n", __func__, sec_build_info);
        pr_emerg("(%s) rebooting...\n", __func__);
        flush_cache_all();
        outer_flush_all();
        msm_restart(0, "peripheral_hw_reset");

        while (1)
                ;
}
EXPORT_SYMBOL(sec_peripheral_secure_check_fail);
#endif

#ifdef CONFIG_SEC_DEBUG_LOW_LOG
unsigned sec_debug_get_reset_reason(void)
{
return reset_reason;
}
#endif
static int sec_debug_panic_handler(struct notifier_block *nb,
		unsigned long l, void *buf)
{
	unsigned int len, i;
	emerg_pet_watchdog();//CTC-should be modify
	sec_debug_set_upload_magic(0x776655ee);

 	len = strnlen(buf, 15);
	if (!strncmp(buf, "User Fault", len))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_USER_FAULT);
	else if (!strncmp(buf, "Crash Key", len))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_FORCED_UPLOAD);
	else if (!strncmp(buf, "CP Crash", len))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_CP_ERROR_FATAL);
	else if (!strncmp(buf, "MDM Crash", len))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_MDM_ERROR_FATAL);
	else if (strnstr(buf, "external_modem", len) != NULL)
		sec_debug_set_upload_cause(UPLOAD_CAUSE_MDM_ERROR_FATAL);
	else if (strnstr(buf, "modem", len) != NULL)
		sec_debug_set_upload_cause(UPLOAD_CAUSE_MODEM_RST_ERR);
	else if (strnstr(buf, "riva", len) != NULL)
		sec_debug_set_upload_cause(UPLOAD_CAUSE_RIVA_RST_ERR);
	else if (strnstr(buf, "lpass", len) != NULL)
		sec_debug_set_upload_cause(UPLOAD_CAUSE_LPASS_RST_ERR);
	else if (strnstr(buf, "dsps", len) != NULL)
		sec_debug_set_upload_cause(UPLOAD_CAUSE_DSPS_RST_ERR);
	else if (!strnicmp(buf, "subsys", len))
		sec_debug_set_upload_cause(UPLOAD_CAUSE_PERIPHERAL_ERR);
	else
		sec_debug_set_upload_cause(UPLOAD_CAUSE_KERNEL_PANIC);

	if (!enable) {
#ifdef CONFIG_SEC_DEBUG_LOW_LOG
		sec_debug_hw_reset();
#endif
		return -EPERM;
	}	

/* enable after SSR feature
	ssr_panic_handler_for_sec_dbg();
*/
	for (i = 0; i < 10; i++) 
	{
	   touch_nmi_watchdog();
	   mdelay(100);
	}

	sec_debug_dump_stack();
	sec_debug_hw_reset();
	return 0;
}

void sec_debug_prepare_for_wdog_bark_reset(void)
{
        if(sec_debug_is_enabled())
        {
                sec_debug_set_upload_magic(0x776655ee);
                sec_debug_set_upload_cause(UPLOAD_CAUSE_NON_SECURE_WDOG_BARK);
        }
}

/*
 * Called from dump_stack()
 * This function call does not necessarily mean that a fatal error
 * had occurred. It may be just a warning.
 */
int sec_debug_dump_stack(void)
{
	if (!enable)
		return -EPERM;

	sec_debug_save_context();

	/* flush L1 from each core.
	   L2 will be flushed later before reset. */
	flush_cache_all();
	return 0;
}
EXPORT_SYMBOL(sec_debug_dump_stack);

#ifdef CONFIG_TOUCHSCREEN_MMS252// debug for tsp ghost touch
extern void dump_tsp_log(void);
#endif

#if defined(CONFIG_TOUCHSCREEN_IST30XX)
extern void tsp_start_read_rawdata(void);
extern void tsp_stop_read_rawdata(void);
#endif

void sec_debug_check_crash_key(unsigned int code, int value)
{
	static enum { NONE, STEP1, STEP2, STEP3} state = NONE;
	if (code == KEY_POWER) {
		if (value)
			sec_debug_set_upload_cause(UPLOAD_CAUSE_POWER_LONG_PRESS);
		else
			sec_debug_set_upload_cause(UPLOAD_CAUSE_INIT);
	}

	if (!enable)
		return;

	switch (state) {
	case NONE:
		if (code == KEY_VOLUMEDOWN && value)
			state = STEP1;
		else
			state = NONE;
		break;
	case STEP1:
		if (code == KEY_POWER && value)
			state = STEP2;
		else
			state = NONE;
		break;
	case STEP2:
		if (code == KEY_POWER && !value)
			state = STEP3;
		else
			state = NONE;
		break;
	case STEP3:
		if (code == KEY_POWER && value) {
			emerg_pet_watchdog();
			dump_all_task_info();
			dump_cpu_stat();
			panic("Crash Key");
		} else {
			state = NONE;
		}
		break;
	}
}

static struct notifier_block nb_reboot_block = {
	.notifier_call = sec_debug_normal_reboot_handler
};

static struct notifier_block nb_panic_block = {
	.notifier_call = sec_debug_panic_handler,
	.priority = -1,
};

static void sec_debug_set_build_info(void)
{
	char *p = sec_build_info;
	strlcat(p, "Kernel Build Info : ", sizeof(sec_build_info));
	strlcat(p, "Date:", sizeof(sec_build_info));
	strlcat(p, sec_build_time[0], sizeof(sec_build_info));
	strlcat(p, "Time:", sizeof(sec_build_info));
	strlcat(p, sec_build_time[1], sizeof(sec_build_info));
}

static int __init __init_sec_debug_log(void)
{
	int i;
	struct sec_debug_log *vaddr;
	int size;

	if (secdbg_paddr == 0 || secdbg_size == 0) {
		size = sizeof(struct sec_debug_log);
		vaddr = kmalloc(size, GFP_KERNEL);
	} else {
		size = secdbg_size;
		vaddr = ioremap_nocache(secdbg_paddr, secdbg_size);
	}

	if ((vaddr == NULL) || (sizeof(struct sec_debug_log) > size)) {
		return -EFAULT;
	}

	memset(vaddr->sched, 0x0, sizeof(vaddr->sched));
	memset(vaddr->irq, 0x0, sizeof(vaddr->irq));
	memset(vaddr->irq_exit, 0x0, sizeof(vaddr->irq_exit));
	memset(vaddr->timer_log, 0x0, sizeof(vaddr->timer_log));
	memset(vaddr->secure, 0x0, sizeof(vaddr->secure));
#ifdef CONFIG_SEC_DEBUG_MSGLOG
	memset(vaddr->secmsg, 0x0, sizeof(vaddr->secmsg));
#endif
#ifdef CONFIG_SEC_DEBUG_AVC_LOG
	memset(vaddr->secavc, 0x0, sizeof(vaddr->secavc));
#endif

	for (i = 0; i < CONFIG_NR_CPUS; i++) {
		atomic_set(&(vaddr->idx_sched[i]), -1);
		atomic_set(&(vaddr->idx_irq[i]), -1);
		atomic_set(&(vaddr->idx_secure[i]), -1);
		atomic_set(&(vaddr->idx_irq_exit[i]), -1);
		atomic_set(&(vaddr->idx_timer[i]), -1);

#ifdef CONFIG_SEC_DEBUG_MSG_LOG
		atomic_set(&(vaddr->idx_secmsg[i]), -1);
#endif
#ifdef CONFIG_SEC_DEBUG_AVC_LOG
		atomic_set(&(vaddr->idx_secavc[i]), -1);
#endif
	}
#ifdef CONFIG_SEC_DEBUG_DCVS_LOG
	for (i = 0; i < CONFIG_NR_CPUS; i++)
		atomic_set(&(vaddr->dcvs_log_idx[i]), -1);
#endif
#ifdef CONFIG_SEC_DEBUG_FUELGAUGE_LOG
		atomic_set(&(vaddr->fg_log_idx), -1);
#endif

	secdbg_log = vaddr;

	return 0;
}

#ifdef CONFIG_SEC_DEBUG_SUBSYS
int sec_debug_save_die_info(const char *str, struct pt_regs *regs)
{
	if (!secdbg_krait)
		return -ENOMEM;
	snprintf(secdbg_krait->excp.pc_sym, sizeof(secdbg_krait->excp.pc_sym),
		"%pS", (void *)regs->ARM_pc);
	snprintf(secdbg_krait->excp.lr_sym, sizeof(secdbg_krait->excp.lr_sym),
		"%pS", (void *)regs->ARM_lr);

	return 0;
}

int sec_debug_save_panic_info(const char *str, unsigned int caller)
{
	if (!secdbg_krait)
		return -ENOMEM;
	snprintf(secdbg_krait->excp.panic_caller,
		sizeof(secdbg_krait->excp.panic_caller), "%pS", (void *)caller);
	snprintf(secdbg_krait->excp.panic_msg,
		sizeof(secdbg_krait->excp.panic_msg), "%s", str);
	snprintf(secdbg_krait->excp.thread,
		sizeof(secdbg_krait->excp.thread), "%s:%d", current->comm,
		task_pid_nr(current));

	return 0;
}

int sec_debug_subsys_add_infomon(char *name, unsigned int size, unsigned int pa)
{
	if (!secdbg_krait)
		return -ENOMEM;

	if (secdbg_krait->info_mon.idx >= ARRAY_SIZE(secdbg_krait->info_mon.var))
		return -ENOMEM;

	strlcpy(secdbg_krait->info_mon.var[secdbg_krait->info_mon.idx].name,
		name, sizeof(secdbg_krait->info_mon.var[0].name));
	secdbg_krait->info_mon.var[secdbg_krait->info_mon.idx].sizeof_type
		= size;
	secdbg_krait->info_mon.var[secdbg_krait->info_mon.idx].var_paddr = pa;

	secdbg_krait->info_mon.idx++;

	return 0;
}

int sec_debug_subsys_add_varmon(char *name, unsigned int size, unsigned int pa)
{
	if (!secdbg_krait)
		return -ENOMEM;

	if (secdbg_krait->var_mon.idx > ARRAY_SIZE(secdbg_krait->var_mon.var))
		return -ENOMEM;

	strlcpy(secdbg_krait->var_mon.var[secdbg_krait->var_mon.idx].name, name,
		sizeof(secdbg_krait->var_mon.var[0].name));
	secdbg_krait->var_mon.var[secdbg_krait->var_mon.idx].sizeof_type = size;
	secdbg_krait->var_mon.var[secdbg_krait->var_mon.idx].var_paddr = pa;

	secdbg_krait->var_mon.idx++;

	return 0;
}

#ifdef CONFIG_SEC_DEBUG_MDM_FILE_INFO
void sec_set_mdm_subsys_info(char *str_buf)
{
	snprintf(secdbg_krait->mdmerr_info,
		sizeof(secdbg_krait->mdmerr_info), "%s", str_buf);
}
#endif
static int ___build_root_init(char *str)
{
	char *st, *ed;
	int len;
	ed = strstr(str, "/android/kernel");
	if (!ed || ed == str)
		return -1;
	*ed = '\0';
	st = strrchr(str, '/');
	if (!st)
		return -1;
	st++;
	len = (unsigned long)ed - (unsigned long)st + 1;
	memmove(str, st, len);
	return 0;
}

#ifdef CONFIG_SEC_DEBUG_VERBOSE_SUMMARY_HTML
void sec_debug_save_cpu_freq_voltage(int cpu, int flag, unsigned long value)
{
	if(SAVE_FREQ == flag)
		cpu_frequency[cpu] = value;
	else if(SAVE_VOLT == flag)
		cpu_volt[cpu] = (unsigned int)value;
}
#else
void sec_debug_save_cpu_freq_voltage(int cpu, int flag, unsigned long value)
{
}
#endif
void sec_debug_secure_app_addr_size(uint32_t addr,uint32_t size)
{
	tzapps_start_addr = addr;
	tzapps_size =  size;
}

int sec_debug_subsys_init(void)
{
#ifdef CONFIG_SEC_DEBUG_VERBOSE_SUMMARY_HTML
	short i;
#endif
	/* paddr of last_pet and last_ns */
	unsigned int last_pet_paddr;
	unsigned int last_ns_paddr;
	char * kernel_cmdline;

	last_pet_paddr = 0;
	last_ns_paddr = 0;

	secdbg_subsys = (struct sec_debug_subsys *)smem_alloc2(
		SMEM_ID_VENDOR2,
		sizeof(struct sec_debug_subsys));

	if (secdbg_subsys == NULL) {
		return -ENOMEM;
	}

	memset(secdbg_subsys, 0, sizeof(secdbg_subsys));
	
	secdbg_subsys->secure_app_start_addr = tzapps_start_addr;
	secdbg_subsys->secure_app_size = tzapps_size;

	secdbg_krait = &secdbg_subsys->priv.krait;

	secdbg_subsys->krait = (struct sec_debug_subsys_data_krait *)(
		(unsigned int)&secdbg_subsys->priv.krait -
		(unsigned int)MSM_SHARED_RAM_BASE + msm_shared_ram_phys);
	secdbg_subsys->rpm = (struct sec_debug_subsys_data *)(
		(unsigned int)&secdbg_subsys->priv.rpm -
		(unsigned int)MSM_SHARED_RAM_BASE + msm_shared_ram_phys);
	secdbg_subsys->modem = (struct sec_debug_subsys_data_modem *)(
		(unsigned int)&secdbg_subsys->priv.modem -
		(unsigned int)MSM_SHARED_RAM_BASE + msm_shared_ram_phys);
	secdbg_subsys->dsps = (struct sec_debug_subsys_data *)(
		(unsigned int)&secdbg_subsys->priv.dsps -
		(unsigned int)MSM_SHARED_RAM_BASE + msm_shared_ram_phys);

	strlcpy(secdbg_krait->name, "Krait", sizeof(secdbg_krait->name) + 1);
	strlcpy(secdbg_krait->state, "Init", sizeof(secdbg_krait->state) + 1);
	secdbg_krait->nr_cpus = CONFIG_NR_CPUS;

	sec_debug_subsys_set_kloginfo(&secdbg_krait->log.idx_paddr,
		&secdbg_krait->log.log_paddr, &secdbg_krait->log.size);
	sec_debug_subsys_set_logger_info(&secdbg_krait->logger_log);

	secdbg_krait->tz_core_dump =
		(struct tzbsp_dump_buf_s **)get_wdog_regsave_paddr();
	ADD_STR_TO_INFOMON(unit_name);
	ADD_VAR_TO_INFOMON(system_rev);
	if (___build_root_init(build_root) == 0)
		ADD_STR_TO_INFOMON(build_root);
	ADD_STR_TO_INFOMON(linux_banner);

	kernel_cmdline = saved_command_line;
	ADD_STR_TO_INFOMON(kernel_cmdline);

#if 0
#ifdef CONFIG_ARCH_MSM8974
	ADD_VAR_TO_VARMON(boost_uv);
	ADD_VAR_TO_VARMON(speed_bin);
	ADD_VAR_TO_VARMON(pvs_bin);
#endif
#endif 

#ifdef CONFIG_ARCH_MSM8974PRO
	ADD_VAR_TO_VARMON(pmc8974_rev);
#else
	ADD_VAR_TO_VARMON(pm8941_rev);
	ADD_VAR_TO_VARMON(pm8841_rev);

	/* save paddrs of last_pet und last_ns */
	if(secdbg_paddr && secdbg_log) {
		last_pet_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, last_pet);
		last_ns_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, last_ns);
		sec_debug_subsys_add_varmon("last_pet", 
				sizeof((secdbg_log->last_pet)),last_pet_paddr);
		sec_debug_subsys_add_varmon("last_ns",
				sizeof((secdbg_log->last_ns.counter)), last_ns_paddr);
	}
	else
		pr_emerg("**** secdbg_log or secdbg_paddr is not initialized ****\n");

#ifdef CONFIG_SEC_DEBUG_VERBOSE_SUMMARY_HTML
	for(i = 0; i < CONFIG_NR_CPUS; i++) {
		ADD_STR_ARRAY_TO_VARMON(cpu_state[i], i, CPU_STAT_CORE);
		ADD_ARRAY_TO_VARMON(cpu_frequency[i],i, CPU_FREQ_CORE);
		ADD_ARRAY_TO_VARMON(cpu_volt[i], i, CPU_VOLT_CORE);
	}
#endif
#endif

	if (secdbg_paddr) {
		secdbg_krait->sched_log.sched_idx_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, idx_sched);
		secdbg_krait->sched_log.sched_buf_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, sched);
		secdbg_krait->sched_log.sched_struct_sz =
			sizeof(struct sched_log);
		secdbg_krait->sched_log.sched_array_cnt = SCHED_LOG_MAX;

		secdbg_krait->sched_log.irq_idx_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, idx_irq);
		secdbg_krait->sched_log.irq_buf_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, irq);
		secdbg_krait->sched_log.irq_struct_sz =
			sizeof(struct irq_log);
		secdbg_krait->sched_log.irq_array_cnt = SCHED_LOG_MAX;

		secdbg_krait->sched_log.secure_idx_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, idx_secure);
		secdbg_krait->sched_log.secure_buf_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, secure);
		secdbg_krait->sched_log.secure_struct_sz =
			sizeof(struct secure_log);
		secdbg_krait->sched_log.secure_array_cnt = TZ_LOG_MAX;

		secdbg_krait->sched_log.irq_exit_idx_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, idx_irq_exit);
		secdbg_krait->sched_log.irq_exit_buf_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, irq_exit);
		secdbg_krait->sched_log.irq_exit_struct_sz =
			sizeof(struct irq_exit_log);
		secdbg_krait->sched_log.irq_exit_array_cnt = SCHED_LOG_MAX;

#ifdef CONFIG_SEC_DEBUG_MSG_LOG
		secdbg_krait->sched_log.msglog_idx_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, idx_secmsg);
		secdbg_krait->sched_log.msglog_buf_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, secmsg);
		secdbg_krait->sched_log.msglog_struct_sz =
			sizeof(struct secmsg_log);
		secdbg_krait->sched_log.msglog_array_cnt = MSG_LOG_MAX;
#else
		secdbg_krait->sched_log.msglog_idx_paddr = 0;
		secdbg_krait->sched_log.msglog_buf_paddr = 0;
		secdbg_krait->sched_log.msglog_struct_sz = 0;
		secdbg_krait->sched_log.msglog_array_cnt = 0;
#endif

#ifdef CONFIG_SEC_DEBUG_AVC_LOG
		secdbg_krait->avc_log.secavc_idx_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, idx_secavc);
		secdbg_krait->avc_log.secavc_buf_paddr = secdbg_paddr +
			offsetof(struct sec_debug_log, secavc);
		secdbg_krait->avc_log.secavc_struct_sz =
			sizeof(struct secavc_log);
		secdbg_krait->avc_log.secavc_array_cnt = AVC_LOG_MAX;
#else
		secdbg_krait->avc_log.secavc_idx_paddr = 0;
		secdbg_krait->avc_log.secavc_buf_paddr = 0;
		secdbg_krait->avc_log.secavc_struct_sz = 0;
		secdbg_krait->avc_log.secavc_array_cnt = 0;
#endif
	}

	/* fill magic nubmer last to ensure data integrity when the magic
	 * numbers are written
	 */
	secdbg_subsys->magic[0] = SEC_DEBUG_SUBSYS_MAGIC0;
	secdbg_subsys->magic[1] = SEC_DEBUG_SUBSYS_MAGIC1;
	secdbg_subsys->magic[2] = SEC_DEBUG_SUBSYS_MAGIC2;
	secdbg_subsys->magic[3] = SEC_DEBUG_SUBSYS_MAGIC3;
	return 0;
}
late_initcall(sec_debug_subsys_init);
#endif

static int parse_address(char* str_address, unsigned *paddress, const char *caller_name)
{
	char * str = str_address;
        unsigned addr = memparse(str, &str);

        //if the memparse succeeded
        if((int)str > (int) str_address)
        {
		unsigned int data;
                if (addr < PAGE_OFFSET || addr > -256UL)
                {
			pr_emerg("%s: Trying to access a non-kernel address 0x%X. This option is currently not implemented.\n", caller_name, addr);
                        return -EACCES;
                }

                if(probe_kernel_address(addr, data))
                {
			pr_emerg("%s: Cannot access the address: 0x%X", caller_name, addr);
                        return -EACCES;
                }

                *paddress = addr;
	}
        else
        {
		pr_emerg("%s: Invalid memory address: %s\n", __func__, str_address);
                return -EINVAL;
        }

	return 0;
}

struct dentry *mem_access_lookup(struct inode *dir, struct dentry *dentry,
                struct nameidata *nd)
{
	struct inode *inode = NULL;
	unsigned address = 0;
	int error = -ENOENT;

	if(!mem_access_file_de)
		goto out;

	pr_emerg("%s: filename: %s", __func__, dentry->d_iname);

	if((dentry->d_name.len == sizeof(MEM_ADDRESS_FILE_NAME)-1) &&
	(!memcmp(dentry->d_name.name, MEM_ADDRESS_FILE_NAME, dentry->d_name.len)))
	{
		//do nothing
		//the default file is accessed.
	}
	else
	{
		error = parse_address((char *) dentry->d_iname, &address, __func__);

		if(error)
		{
			goto out;
		}
	}

	atomic_inc(&mem_access_file_de->count); //equivalent to pde_get

	error = -EINVAL;

	inode = proc_get_inode(dir->i_sb, mem_access_file_de);

	if (inode) {
		d_set_d_op(dentry, &proc_dentry_operations);
		d_add(dentry, inode);
		return NULL;
	}

	pde_put(mem_access_file_de);

out:
	return ERR_PTR(error);
}

static int sec_debug_mem_write(struct file *file, const char __user *buffer,
                size_t count, loff_t *offs)
{
	char local_buf[12];
	char * str = NULL;
	unsigned data;
	unsigned address;
	int error;

	pr_emerg("%s: write %s", __func__, file->f_path.dentry->d_iname);

	if((count > 11) || (buffer == NULL))
	{
		pr_emerg("%s: Invalid parameter\n", __func__);
		return -EINVAL;
	}

	if((file->f_path.dentry->d_name.len == sizeof(MEM_ADDRESS_FILE_NAME)-1) &&
        (!memcmp(file->f_path.dentry->d_name.name, MEM_ADDRESS_FILE_NAME, file->f_path.dentry->d_name.len)))
        {
                //do nothing
                //the default file is accessed.
		return count;
        }

	error = parse_address((char *) file->f_path.dentry->d_iname, &address, __func__);

	if(error)
	{
		return error;
	}

	memcpy(local_buf, buffer, count);
	local_buf[count]=0;

	str = local_buf;

	data = memparse(str, &str);

	//if the memparse succeeded
	if((int)str > (int) local_buf)
	{
		if(probe_kernel_write((void *)address, &data, 4))
		{
			pr_emerg("%s: Unable to write 0x%X to 0x%X\n", __func__, data, address);
			return -EACCES;
		}
	}
	else
	{
		pr_emerg("%s: Invalid data: %s\n", __func__, local_buf);
		return -EINVAL;
	}

	return count;
}

static int sec_debug_mem_read(struct file *file, char __user *buffer,
                size_t count, loff_t *offs)
{
	unsigned data;
	int read_count = 0;
	unsigned address;
        int error;

	static int read_done = 0;

        if((file->f_path.dentry->d_name.len == sizeof(MEM_ADDRESS_FILE_NAME)-1) &&
        (!memcmp(file->f_path.dentry->d_name.name, MEM_ADDRESS_FILE_NAME, file->f_path.dentry->d_name.len)))
        {
                //do nothing
                //the default file is accessed.
                return 0;
        }

	if(read_done)
	{
		read_done = 0;
		return 0;
	}

        if(buffer == NULL)
        {
                return -EINVAL;
	}

	error = parse_address((char *) file->f_path.dentry->d_iname, &address, __func__);

        if(error)
        {
                return error;
        }

        if(probe_kernel_read(&data, (const void *)address, 4))
        {
                pr_emerg("%s: Unable to read from 0x%X\n", __func__, address);
                return -EACCES;
        }
        else
        {
		read_count = sprintf(buffer, "0x%08X\n", data);
		read_done = 1;
        }

	pr_emerg("%s: read %s", __func__, file->f_path.dentry->d_iname);
	return read_count;
}

int __init sec_debug_procfs_init(void)
{
	sec_debug_dir_de = proc_mkdir("sec_debug", NULL);
	if(!sec_debug_dir_de)
	{
		return -ENOMEM;
	}

	mem_access_dir_de = proc_mkdir("memaccess", sec_debug_dir_de);
	if(!mem_access_dir_de)
	{
		remove_proc_entry("sec_debug", NULL);
		sec_debug_dir_de = NULL;
		return -ENOMEM;
	}

	mem_access_proc_dir_inode_operations = *(mem_access_dir_de->proc_iops);
	mem_access_proc_dir_inode_operations.lookup = mem_access_lookup;
	mem_access_dir_de->proc_iops = &mem_access_proc_dir_inode_operations;

	mem_access_file_de = proc_create(MEM_ADDRESS_FILE_NAME, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP, mem_access_dir_de,
			&sec_debug_mem_access_proc_fops);

	if (!mem_access_file_de)
	{
		remove_proc_entry("memaccess", sec_debug_dir_de);
		mem_access_dir_de = NULL;

		remove_proc_entry("sec_debug", NULL);
		sec_debug_dir_de = NULL;

		return -ENOMEM;
	}

        return 0;
}

#ifdef CONFIG_USER_RESET_DEBUG
// SEC_CP_CRASH_LOG
int sec_debug_get_cp_crash_log(char *str)
{
    struct sec_debug_subsys_data_modem *modem = (struct sec_debug_subsys_data_modem *)&secdbg_subsys->priv.modem;

//  if(!strcmp(modem->state, "Init"))
//      return strcpy(str, "There is no cp crash log);

    return sprintf(str, "%s, %s, %s, %d, %s",
        modem->excp.type, modem->excp.task, modem->excp.file, modem->excp.line, modem->excp.msg);
}
#endif /* CONFIG_USER_RESET_DEBUG */

int __init sec_debug_init(void)
{
	restart_reason = MSM_IMEM_BASE + RESTART_REASON_ADDR;
	/* Using bottom of sec_dbg DDR address range for writting restart reason */
#ifdef CONFIG_RESTART_REASON_DDR
		restart_reason_ddr_address = ioremap_nocache(RESTART_REASON_DDR_ADDR, SZ_4K);
		pr_emerg("%s: restart_reason_ddr_address : 0x%x \n", __func__,(unsigned int)restart_reason_ddr_address);
#endif

	pr_emerg("%s: enable=%d\n", __func__, enable);
	pr_emerg("%s:__raw_readl restart_reason=%d\n", __func__, __raw_readl(restart_reason));
	/* check restart_reason here */
	pr_emerg("%s: restart_reason : 0x%x\n", __func__,
		(unsigned int)restart_reason);

	register_reboot_notifier(&nb_reboot_block);
	atomic_notifier_chain_register(&panic_notifier_list, &nb_panic_block);

	if (!enable)
		return -EPERM;

#ifdef CONFIG_SEC_DEBUG_SCHED_LOG
	__init_sec_debug_log();
#endif

	debug_semaphore_init();
	sec_debug_set_build_info();
	sec_debug_set_upload_magic(0x776655ee);
	sec_debug_set_upload_cause(UPLOAD_CAUSE_INIT);

	sec_debug_procfs_init();

	return 0;
}

int sec_debug_is_enabled(void)
{
	return enable;
}

#ifdef CONFIG_SEC_FILE_LEAK_DEBUG

void sec_debug_print_file_list(void)
{
	int i=0;
	unsigned int nCnt=0;
	struct file *file=NULL;
	struct files_struct *files = current->files;
	const char *pRootName=NULL;
	const char *pFileName=NULL;

	nCnt=files->fdt->max_fds;

	printk(KERN_ERR " [Opened file list of process %s(PID:%d, TGID:%d) :: %d]\n",
		current->group_leader->comm, current->pid, current->tgid,nCnt);

	for (i=0; i<nCnt; i++) {

		rcu_read_lock();
		file = fcheck_files(files, i);

		pRootName=NULL;
		pFileName=NULL;

		if (file) {
			if (file->f_path.mnt
				&& file->f_path.mnt->mnt_root
				&& file->f_path.mnt->mnt_root->d_name.name)
				pRootName=file->f_path.mnt->mnt_root->d_name.name;

			if (file->f_path.dentry && file->f_path.dentry->d_name.name)
				pFileName=file->f_path.dentry->d_name.name;

			printk(KERN_ERR "[%04d]%s%s\n",i,pRootName==NULL?"null":pRootName,
							pFileName==NULL?"null":pFileName);
		}
		rcu_read_unlock();
	}
}

void sec_debug_EMFILE_error_proc(unsigned long files_addr)
{
	if (files_addr!=(unsigned long)(current->files)) {
		printk(KERN_ERR "Too many open files Error at %pS\n"
						"%s(%d) thread of %s process tried fd allocation by proxy.\n"
						"files_addr = 0x%lx, current->files=0x%p\n",
					__builtin_return_address(0),
					current->comm,current->tgid,current->group_leader->comm,
					files_addr, current->files);
		return;
	}

	printk(KERN_ERR "Too many open files(%d:%s) at %pS\n",
		current->tgid, current->group_leader->comm,__builtin_return_address(0));

	if (!enable)
		return;

	/* We check EMFILE error in only "system_server","mediaserver" and "surfaceflinger" process.*/
	if (!strcmp(current->group_leader->comm, "system_server")
		||!strcmp(current->group_leader->comm, "mediaserver")
		||!strcmp(current->group_leader->comm, "surfaceflinger")){
		sec_debug_print_file_list();
		panic("Too many open files");
	}
}
#endif


/* klaatu - schedule log */
#ifdef CONFIG_SEC_DEBUG_SCHED_LOG
void __sec_debug_task_sched_log(int cpu, struct task_struct *task,
						char *msg)
{
	unsigned i;

	if (!secdbg_log)
		return;

	if (!task && !msg)
		return;

	i = atomic_inc_return(&(secdbg_log->idx_sched[cpu]))
		& (SCHED_LOG_MAX - 1);
	secdbg_log->sched[cpu][i].time = cpu_clock(cpu);
	if (task) {
		strlcpy(secdbg_log->sched[cpu][i].comm, task->comm,
			sizeof(secdbg_log->sched[cpu][i].comm));
		secdbg_log->sched[cpu][i].pid = task->pid;
	} else {
		strlcpy(secdbg_log->sched[cpu][i].comm, msg,
			sizeof(secdbg_log->sched[cpu][i].comm));
		secdbg_log->sched[cpu][i].pid = -1;
	}
}

void sec_debug_task_sched_log_short_msg(char *msg)
{
	__sec_debug_task_sched_log(raw_smp_processor_id(), NULL, msg);
}

void sec_debug_task_sched_log(int cpu, struct task_struct *task)
{
	__sec_debug_task_sched_log(cpu, task, NULL);
}

void sec_debug_timer_log(unsigned int type, int int_lock, void *fn)
{
	int cpu = smp_processor_id();
	unsigned i;

	if (!secdbg_log)
		return;

	i = atomic_inc_return(&(secdbg_log->idx_timer[cpu]))
		& (SCHED_LOG_MAX - 1);
	secdbg_log->timer_log[cpu][i].time = cpu_clock(cpu);
	secdbg_log->timer_log[cpu][i].type = type;
	secdbg_log->timer_log[cpu][i].int_lock = int_lock;
	secdbg_log->timer_log[cpu][i].fn = (void *)fn;
}

void sec_debug_secure_log(u32 svc_id,u32 cmd_id)
{
	unsigned long flags;
	static DEFINE_SPINLOCK(secdbg_securelock);
	int cpu ;
	unsigned i;

	spin_lock_irqsave(&secdbg_securelock, flags);
	cpu	= smp_processor_id();
	if (!secdbg_log){
	   spin_unlock_irqrestore(&secdbg_securelock, flags);
	   return;
	}
	i = atomic_inc_return(&(secdbg_log->idx_secure[cpu]))
		& (TZ_LOG_MAX - 1);
	secdbg_log->secure[cpu][i].time = cpu_clock(cpu);
	secdbg_log->secure[cpu][i].svc_id = svc_id;
	secdbg_log->secure[cpu][i].cmd_id = cmd_id ;
	spin_unlock_irqrestore(&secdbg_securelock, flags);
}

void sec_debug_irq_sched_log(unsigned int irq, void *fn, int en)
{
	int cpu = smp_processor_id();
	unsigned i;

	if (!secdbg_log)
		return;

	i = atomic_inc_return(&(secdbg_log->idx_irq[cpu]))
		& (SCHED_LOG_MAX - 1);
	secdbg_log->irq[cpu][i].time = cpu_clock(cpu);
	secdbg_log->irq[cpu][i].irq = irq;
	secdbg_log->irq[cpu][i].fn = (void *)fn;
	secdbg_log->irq[cpu][i].en = en;
	secdbg_log->irq[cpu][i].preempt_count = preempt_count();
	secdbg_log->irq[cpu][i].context = &cpu;
}

#ifdef CONFIG_SEC_DEBUG_IRQ_EXIT_LOG
void sec_debug_irq_enterexit_log(unsigned int irq,
					unsigned long long start_time)
{
	int cpu = smp_processor_id();
	unsigned i;

	if (!secdbg_log)
		return;

	i = atomic_inc_return(&(secdbg_log->idx_irq_exit[cpu]))
		& (SCHED_LOG_MAX - 1);
	secdbg_log->irq_exit[cpu][i].time = start_time;
	secdbg_log->irq_exit[cpu][i].end_time = cpu_clock(cpu);
	secdbg_log->irq_exit[cpu][i].irq = irq;
	secdbg_log->irq_exit[cpu][i].elapsed_time =
		secdbg_log->irq_exit[cpu][i].end_time - start_time;
}
#endif

#ifdef CONFIG_SEC_DEBUG_MSG_LOG
asmlinkage int sec_debug_msg_log(void *caller, const char *fmt, ...)
{
	int cpu = smp_processor_id();
	int r = 0;
	int i;
	va_list args;

	if (!secdbg_log)
		return 0;

	i = atomic_inc_return(&(secdbg_log->idx_secmsg[cpu]))
		& (MSG_LOG_MAX - 1);
	secdbg_log->secmsg[cpu][i].time = cpu_clock(cpu);
	va_start(args, fmt);
	r = vsnprintf(secdbg_log->secmsg[cpu][i].msg,
		sizeof(secdbg_log->secmsg[cpu][i].msg), fmt, args);
	va_end(args);

	secdbg_log->secmsg[cpu][i].caller0 = __builtin_return_address(0);
	secdbg_log->secmsg[cpu][i].caller1 = caller;
	secdbg_log->secmsg[cpu][i].task = current->comm;

	return r;
}

#endif

#ifdef CONFIG_SEC_DEBUG_AVC_LOG
asmlinkage int sec_debug_avc_log(const char *fmt, ...)
{
	int cpu = smp_processor_id();
	int r = 0;
	int i;
	va_list args;

	if (!secdbg_log)
		return 0;

	i = atomic_inc_return(&(secdbg_log->idx_secavc[cpu]))
		& (AVC_LOG_MAX - 1);
	va_start(args, fmt);
	r = vsnprintf(secdbg_log->secavc[cpu][i].msg,
		sizeof(secdbg_log->secavc[cpu][i].msg), fmt, args);
	va_end(args);

	return r;
}

#endif
#endif	/* CONFIG_SEC_DEBUG_SCHED_LOG */

static int __init sec_dbg_setup(char *str)
{
	unsigned size = memparse(str, &str);

	pr_emerg("%s: str=%s\n", __func__, str);

	if (size && (size == roundup_pow_of_two(size)) && (*str == '@')) {
		secdbg_paddr = (unsigned int)memparse(++str, NULL);
		secdbg_size = size;
	}

	pr_emerg("%s: secdbg_paddr = 0x%x\n", __func__, secdbg_paddr);
	pr_emerg("%s: secdbg_size = 0x%x\n", __func__, secdbg_size);

	return 1;
}

__setup("sec_dbg=", sec_dbg_setup);


static void sec_user_fault_dump(void)
{
	if (enable == 1 && enable_user == 1)
		panic("User Fault");
}

static int sec_user_fault_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *offs)
{
	char buf[100];

	if (count > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (strncmp(buf, "dump_user_fault", 15) == 0)
		sec_user_fault_dump();

	return count;
}

static const struct file_operations sec_user_fault_proc_fops = {
	.write = sec_user_fault_write,
};

static int __init sec_debug_user_fault_init(void)
{
	struct proc_dir_entry *entry;

	entry = proc_create("user_fault", S_IWUSR|S_IWGRP, NULL,
			&sec_user_fault_proc_fops);
	if (!entry)
		return -ENOMEM;
	return 0;
}
device_initcall(sec_debug_user_fault_init);

#ifdef CONFIG_USER_RESET_DEBUG
static int set_reset_reason_proc_show(struct seq_file *m, void *v)
{
	if (reset_reason == RR_S)
		seq_printf(m, "SPON\n");
	else if (reset_reason == RR_W)
		seq_printf(m, "WPON\n");
	else if (reset_reason == RR_D)
		seq_printf(m, "DPON\n");
	else if (reset_reason == RR_K)
		seq_printf(m, "KPON\n");
	else if (reset_reason == RR_M)
		seq_printf(m, "MPON\n");
	else if (reset_reason == RR_P)
		seq_printf(m, "PPON\n");
	else if (reset_reason == RR_R)
		seq_printf(m, "RPON\n");
	else if (reset_reason == RR_B)
		seq_printf(m, "BPON\n");
	else
		seq_printf(m, "NPON\n");

	return 0;
}

static int sec_reset_reason_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, set_reset_reason_proc_show, NULL);
}

static const struct file_operations sec_reset_reason_proc_fops = {
	.open = sec_reset_reason_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init sec_debug_reset_reason_init(void)
{
	struct proc_dir_entry *entry;

	entry = proc_create("reset_reason", S_IWUGO, NULL,
		&sec_reset_reason_proc_fops);

	if (!entry)
		return -ENOMEM;

	return 0;
}

device_initcall(sec_debug_reset_reason_init);
#endif


#ifdef CONFIG_SEC_DEBUG_DCVS_LOG
void sec_debug_dcvs_log(int cpu_no, unsigned int prev_freq,
						unsigned int new_freq)
{
	unsigned int i;
	if (!secdbg_log)
		return;

	i = atomic_inc_return(&(secdbg_log->dcvs_log_idx[cpu_no]))
		& (DCVS_LOG_MAX - 1);
	secdbg_log->dcvs_log[cpu_no][i].cpu_no = cpu_no;
	secdbg_log->dcvs_log[cpu_no][i].prev_freq = prev_freq;
	secdbg_log->dcvs_log[cpu_no][i].new_freq = new_freq;
	secdbg_log->dcvs_log[cpu_no][i].time = cpu_clock(cpu_no);
}
#endif
#ifdef CONFIG_SEC_DEBUG_FUELGAUGE_LOG
void sec_debug_fuelgauge_log(unsigned int voltage, unsigned short soc,
				unsigned short charging_status)
{
	unsigned int i;
	int cpu = smp_processor_id();

	if (!secdbg_log)
		return;

	i = atomic_inc_return(&(secdbg_log->fg_log_idx))
		& (FG_LOG_MAX - 1);
	secdbg_log->fg_log[i].time = cpu_clock(cpu);
	secdbg_log->fg_log[i].voltage = voltage;
	secdbg_log->fg_log[i].soc = soc;
	secdbg_log->fg_log[i].charging_status = charging_status;
}
#endif

#ifdef CONFIG_USER_RESET_DEBUG
// SEC_CP_CRASH_LOG
static int sec_cp_crash_log_proc_show(struct seq_file *m, void *v)
{
    char log_msg[512];

    sec_debug_get_cp_crash_log(log_msg);

    seq_printf(m, log_msg);

    return 0;
}

static int sec_cp_crash_log_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, sec_cp_crash_log_proc_show, NULL);
}

static const struct file_operations sec_cp_crash_log_proc_fops = {
    .open       = sec_cp_crash_log_proc_open,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = single_release,
};

static int __init sec_debug_cp_crash_log_init(void)
{
    struct proc_dir_entry *entry;

    entry = proc_create("cp_crash_log", S_IRUGO, NULL,
                            &sec_cp_crash_log_proc_fops);
    if (!entry)
        return -ENOMEM;

    return 0;
}

device_initcall(sec_debug_cp_crash_log_init);
#endif /* CONFIG_USER_RESET_DEBUG */
