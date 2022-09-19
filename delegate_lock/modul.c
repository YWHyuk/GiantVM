#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/threads.h>
#include <linux/debugfs.h>
#include <linux/kthread.h>
#include <linux/cache.h>
#include <linux/dynamic_debug.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <asm/atomic.h>
#include <asm/processor.h>
#include <asm/barrier.h>
#include <asm/cmpxchg.h>
#include "list_bench.h"
#include "delegate_lock.h"

MODULE_DESCRIPTION("Simple Lock benchmark module");
MODULE_AUTHOR("Wonhyuk Yang");
MODULE_LICENSE("GPL");

#define NR_BENCH	(5000000)
#define NR_SAMPLE	1000

#define MAX_CPU		32
#define MAX_DELAY 	10000

#define GVM_CACHE_BYTES		(1<<12)

static int max_cpus = 31;
static int delay_time = 100;
static int nr_bench = 500000<<1;
static int thread_switch = 0;

typedef void* (*request_t)(void *);
typedef int (*test_thread_t)(void *);

int prepare_tests(test_thread_t, void *, const char *);
int cclock_bench(void *data);
int spinlock_bench(void *data);
int cclist_bench(void *data);
int spinlist_bench(void *data);

struct lb_info {
	request_t req;
	void *params;
	void *lock;
	int counter;
	bool monitor;
	bool quit;
};

DEFINE_PER_CPU(struct lb_info, lb_info_array);
DEFINE_PER_CPU(struct task_struct *, task_array);

static unsigned long perf_result[NR_BENCH/NR_SAMPLE];

/* Dummy workload */
__attribute__((aligned(GVM_CACHE_BYTES))) DEFINE_SPINLOCK(dummy_spinlock);
__attribute__((aligned(GVM_CACHE_BYTES))) struct delegate_spinlock dummy_dlock = __ARCH_SPIN_LOCK_UNLOCKED;
atomic_t dummy_lock __attribute__((aligned(GVM_CACHE_BYTES))) = ATOMIC_INIT(0);
int dummy_counter __attribute__((aligned(GVM_CACHE_BYTES))) = 0;
int cache_table[1024*4+1];
void* dummy_increment(void* params)
{
	int i;
	int *counter = (int*)params;
	(*counter)++;
	return NULL;
	for (i = 0; i < 0; i++)
		cache_table[i*1024]++;

	if (delay_time)
		udelay(delay_time);
	return params;
}


/* Debugfs */
static int
lb_open(struct inode *inode, struct file *filep)
{
	return 0;
}

static int
lb_release(struct inode *inode, struct file *filep)
{
	return 0;
}

static ssize_t lb_write(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	unsigned long val;
	int ret;
	int cpu;
	struct lb_info *li;
	bool monitor_thread = true;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	WRITE_ONCE(thread_switch, 0);
	if (val == 1) {
		for_each_online_cpu(cpu) {
			li = &per_cpu(lb_info_array, cpu);
			li->req = dummy_increment;
			li->params = &dummy_counter;
			li->lock = (void *)&dummy_dlock;
			li->counter = nr_bench;
			li->monitor = monitor_thread;
			monitor_thread = false;
		}
		prepare_tests(cclock_bench, (void *)&lb_info_array, "d-lockbench");
	} else if (val == 2) {
		for_each_online_cpu(cpu) {
			li = &per_cpu(lb_info_array, cpu);
			li->req = dummy_increment;
			li->params = &dummy_counter;
			li->lock = (void *)&dummy_spinlock;
			li->counter = nr_bench;
			li->monitor = monitor_thread;
			monitor_thread = false;
		}
		prepare_tests(spinlock_bench, (void *)&lb_info_array, "spinlockbench");
	}
	else if (val == 3) {
		for_each_online_cpu(cpu) {
			li = &per_cpu(lb_info_array, cpu);
			li->lock = (void *)&dummy_dlock;
			li->counter = nr_bench;
			li->monitor = monitor_thread;
			monitor_thread = false;
		}
		prepare_tests(cclist_bench, (void *)&lb_info_array, "d-list");
	} else if (val == 4) {
		for_each_online_cpu(cpu) {
			li = &per_cpu(lb_info_array, cpu);
			li->lock = (void *)&dummy_spinlock;
			li->counter = nr_bench;
			li->monitor = monitor_thread;
			monitor_thread = false;
		}
		prepare_tests(spinlist_bench, (void *)&lb_info_array, "spin-list");
	}

	(*ppos)++;
	udelay(1000);
	WRITE_ONCE(thread_switch, 1);
	return cnt;
}

static ssize_t lb_quit(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	unsigned long val;
	int ret, cpu;
	struct lb_info *ld;
	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);

	if (ret)
		return ret;

	if (val == 1) {
		dummy_lock.counter = 0;
		for_each_online_cpu(cpu) {
			ld = &per_cpu(lb_info_array, cpu);
			ld->quit = true;

		}
	}
	(*ppos)++;
	return cnt;
}

static ssize_t lb_cpu(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	int ret;
	unsigned long val;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);

	if (ret)
		return ret;

	if (val > 0 && val < MAX_CPU)
		max_cpus = val;

	(*ppos)++;
	return cnt;
}

static ssize_t lb_bench(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	int ret;
	unsigned long val;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);

	if (ret)
		return ret;

	if (val > 0 && val <= NR_BENCH)
		nr_bench = val;

	(*ppos)++;
	return cnt;
}

static ssize_t lb_delay(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	int ret;
	unsigned long val;
	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);

	if (ret)
		return ret;
	if (val >= 0 && val <= MAX_DELAY)
		delay_time = val;
	(*ppos)++;
	return cnt;
}

static void *t_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *t_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void t_stop(struct seq_file *m, void *v)
{
}

static int t_show(struct seq_file *m, void *v)
{
	return 0;
}

static const struct seq_operations show_status_seq_ops= {
	.start		= t_start,
	.next		= t_next,
	.stop		= t_stop,
	.show		= t_show,
};

static int lb_status_open(struct inode *inode, struct file *file)
{
	struct seq_file *m;
	int ret;

	ret = seq_open(file, &show_status_seq_ops);
	if (ret) {
		return ret;
	}

	m = file->private_data;
	return 0;
}

static int lb_status_release(struct inode *inode, struct file *file)
{
	return seq_release(inode, file);
}

static int r_show(struct seq_file *m, void *v)
{
	int cpu;
	struct task_struct *thread;

	for_each_online_cpu(cpu) {
		thread = per_cpu(task_array, cpu);
		if (thread) {
			seq_printf(m, "0");
			return 0;
		}
	}
	seq_printf(m, "1");
	return 0;
}

static const struct seq_operations show_ready_seq_ops= {
	.start		= t_start,
	.next		= t_next,
	.stop		= t_stop,
	.show		= r_show,
};

static int lb_ready_open(struct inode *inode, struct file *file)
{
	struct seq_file *m;
	int ret;

	ret = seq_open(file, &show_ready_seq_ops);
	if (ret) {
		return ret;
	}

	m = file->private_data;
	return 0;
}

static int lb_ready_release(struct inode *inode, struct file *file)
{
	return seq_release(inode, file);
}

static const struct file_operations lb_trigger_fops = {
	.open	 = lb_open,
	.read	 = NULL,
	.write   = lb_write,
	.release = lb_release,
	.llseek  = NULL,
};

static const struct file_operations lb_quit_fops = {
	.open	 = lb_open,
	.write   = lb_quit,
	.release = lb_release,
};

static const struct file_operations lb_cpu_fops = {
	.open	 = lb_open,
	.write   = lb_cpu,
	.release = lb_release,
};

static const struct file_operations lb_bench_fops = {
	.open	 = lb_open,
	.write   = lb_bench,
	.release = lb_release,
};

static const struct file_operations lb_delay_fops = {
	.open	 = lb_open,
	.write   = lb_delay,
	.release = lb_release,
};

static const struct file_operations lb_status_fops= {
	.open	 = lb_status_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = lb_status_release,
};

static const struct file_operations lb_ready_fops= {
	.open	 = lb_ready_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = lb_ready_release,
};

static struct dentry *lb_debugfs_root;

static int lb_debugfs_init(void)
{
	lb_debugfs_root = debugfs_create_dir("lock_benchmark", NULL);

	debugfs_create_file("trigger", 0200,
					lb_debugfs_root, NULL, &lb_trigger_fops);
	debugfs_create_file("quit", 0200,
					lb_debugfs_root, NULL, &lb_quit_fops);
	debugfs_create_file("cpu", 0200,
					lb_debugfs_root, NULL, &lb_cpu_fops);
	debugfs_create_file("nr_bench", 0200,
					lb_debugfs_root, NULL, &lb_bench_fops);
	debugfs_create_file("delay", 0200,
					lb_debugfs_root, NULL, &lb_delay_fops);
	debugfs_create_file("status", 0400,
					lb_debugfs_root, NULL, &lb_status_fops);
	debugfs_create_file("ready", 0400,
					lb_debugfs_root, NULL, &lb_ready_fops);


	return 0;
}

static int lb_debugfs_exit(void)
{
	debugfs_remove_recursive(lb_debugfs_root);
	return 0;
}

int cclock_bench(void *data)
{
	int i;
	int cpu = get_cpu();
	unsigned long flags;
	struct lb_info *lb_data = &per_cpu(lb_info_array, cpu);
	unsigned long prev = 0, cur;
	while(!READ_ONCE(thread_switch));

	if (unlikely(lb_data->monitor))
		prev = sched_clock();
	for (i=0; i<lb_data->counter && !READ_ONCE(lb_data->quit);i++) {
		if (unlikely(lb_data->monitor && i && (i%NR_SAMPLE==0))) {
			cur = sched_clock();
			perf_result[(i/NR_SAMPLE)-1] = cur-prev;
			prev = cur;
		}
		//local_irq_save(flags);
		__delegate_run(lb_data->lock, lb_data->req, lb_data->params);
		//local_irq_restore(flags);
	}

	if (unlikely(lb_data->monitor)) {
		cur = sched_clock();
		perf_result[(i+NR_SAMPLE-1)/NR_SAMPLE-1] = cur-prev;
		for (i=0;i<lb_data->counter;i+=NR_SAMPLE)
			printk("lockbench: <cc-lock> monitor thread %dth [%lu]\n", i, perf_result[i/NR_SAMPLE]);
	}

	per_cpu(task_array, cpu) = NULL;
	put_cpu();
	return 0;
}
static inline void profile_spin(spinlock_t *lock, struct lb_info * lb_data) {
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	lb_data->req(lb_data->params);
	spin_unlock_irqrestore(lock, flags);
}

int spinlock_bench(void *data)
{
	int i;
	int cpu = get_cpu();
	struct lb_info * lb_data = &per_cpu(*((struct lb_info *)data), cpu);
	unsigned long prev = 0, cur = 0;

	spinlock_t *lock = (spinlock_t *)lb_data->lock;
	while(!READ_ONCE(thread_switch));

	if (unlikely(lb_data->monitor))
		prev = sched_clock();
	for (i=0; i<lb_data->counter && !READ_ONCE(lb_data->quit); i++) {
		if (unlikely(lb_data->monitor && i && (i%NR_SAMPLE==0))) {
			cur = sched_clock();
			perf_result[(i/NR_SAMPLE)-1] = cur-prev;
			prev = cur;
		}
		profile_spin(lock, lb_data);
	}

	if (unlikely(lb_data->monitor)) {
		cur = sched_clock();
		perf_result[(i+NR_SAMPLE-1)/NR_SAMPLE-1] = cur-prev;
		for (i=0;i<lb_data->counter;i+=NR_SAMPLE)
			printk("lockbench: <spinlock> monitor thread %dth [%lu]\n", i, perf_result[i/NR_SAMPLE]);
	}

	per_cpu(task_array, cpu) = NULL;
	put_cpu();
	return 0;
}

int cclist_bench(void *data)
{
	int i, j;
	int cpu = get_cpu();
	unsigned long flags;
	struct lb_info *lb_data = &per_cpu(lb_info_array, cpu);
	struct list_head *list_node = kmalloc(sizeof(struct list_head)*LIST_LEN, GFP_KERNEL);
	struct list_param param;
	unsigned long prev = 0, cur;
	while(!READ_ONCE(thread_switch));

	if (unlikely(lb_data->monitor))
		prev = sched_clock();
	for (i=0; i<lb_data->counter && !READ_ONCE(lb_data->quit);i++) {
		if (unlikely(lb_data->monitor && i)) {
			cur = sched_clock();
			perf_result[i] = cur-prev;
			prev = cur;
		}

		param.arg[1] = &cc_head;
		for(j=0;j<LIST_LEN;j++) {
			param.arg[0] = list_node+j;

			local_irq_save(flags);
			__delegate_run(lb_data->lock, cc_list_add, &param);
			local_irq_restore(flags);
			udelay(5);
		}

		for(j=0;j<LIST_LEN;j++) {
			param.arg[0] = list_node+j;

			local_irq_save(flags);
			__delegate_run(lb_data->lock, cc_list_del, &param);
			local_irq_restore(flags);
			udelay(5);
		}
	}

	if (unlikely(lb_data->monitor)) {
		cur = sched_clock();
		perf_result[i] = cur-prev;
		for (i=0;i<lb_data->counter;i++)
			printk("lockbench: <cc-lock> monitor thread %dth [%lu]\n", i, perf_result[i]);
	}
	kfree(list_node);
	per_cpu(task_array, cpu) = NULL;
	put_cpu();
	return 0;
}

int spinlist_bench(void *data)
{
	int i, j;
	int cpu = get_cpu();
	unsigned long flags;
	struct lb_info *lb_data = &per_cpu(lb_info_array, cpu);
	spinlock_t *lock = (spinlock_t *)lb_data->lock;
	
	struct list_head *list_node = kmalloc(sizeof(struct list_head)*LIST_LEN, GFP_KERNEL);
	unsigned long prev = 0, cur;
	while(!READ_ONCE(thread_switch));

	if (unlikely(lb_data->monitor))
		prev = sched_clock();
	for (i=0; i<lb_data->counter && !READ_ONCE(lb_data->quit);i++) {
		if (unlikely(lb_data->monitor && i)) {
			cur = sched_clock();
			perf_result[i] = cur-prev;
			prev = cur;
		}

		for(j=0;j<LIST_LEN;j++) {
			spin_lock_irqsave(lock, flags);
			list_add(list_node+j, &spin_head);
			spin_unlock_irqrestore(lock, flags);
			udelay(5);
		}

		for(j=0;j<LIST_LEN;j++) {
			spin_lock_irqsave(lock, flags);
			list_del(list_node+j);
			spin_unlock_irqrestore(lock, flags);
			udelay(5);
		}
	}

	if (unlikely(lb_data->monitor)) {
		cur = sched_clock();
		perf_result[i] = cur-prev;
		for (i=0;i<lb_data->counter;i++)
			printk("lockbench: <spin-lock> monitor thread %dth [%lu]\n", i, perf_result[i]);
	}
	kfree(list_node);
	per_cpu(task_array, cpu) = NULL;
	put_cpu();
	return 0;
}

int prepare_tests(test_thread_t test, void *arg, const char *name)
{
	struct task_struct *thread;
	int cpu;
	int nr_cpus = 0;

	for_each_online_cpu(cpu) {
		thread = per_cpu(task_array, cpu);
		if (thread != NULL) {
			pr_debug("lockbench: test is progressing!\n");
			return 1;
		}
	}

	for_each_online_cpu(cpu) {
		if (nr_cpus++ >= max_cpus)
			break;

		thread = kthread_create(test, arg, "%s/%u", name, cpu);
		if (IS_ERR(thread)) {
			pr_err("Failed to create kthread on CPU %u\n", cpu);
			continue;
		}
		kthread_bind(thread, cpu);

		wake_up_process(thread);
		per_cpu(task_array, cpu) = thread;
	}
	return 0;
}

/* module init/exit */
static int lock_benchmark_init(void)
{
	lb_debugfs_init();
	return 0;
}

static void lock_benchmark_exit(void)
{
	lb_debugfs_exit();
}
module_init(lock_benchmark_init);
module_exit(lock_benchmark_exit);
