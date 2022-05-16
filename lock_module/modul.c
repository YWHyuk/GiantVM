#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
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

MODULE_DESCRIPTION("Simple Lock benchmark module");
MODULE_AUTHOR("Wonhyuk Yang");
MODULE_LICENSE("GPL");

#define DEBUG
#define NR_BENCH (500000)

#define MAX_COMBINER_OPERATIONS 5

#define MAX_CPU		16
#define MAX_DELAY 	100

static int max_cpus = 2;
static int delay_time = 0;
static int nr_bench = 500000;

/* CPU number and index are encoded in cc_node.next
 * Now, each cpu has two node and these nodes are
 * used alternatley. In this way, we can avoid
 * node overwrite problem.
 */
#define INDEX_SHIFT			(1)
#define INDEX_MASK			((1 << INDEX_SHIFT) - 1)
#define ENCODE_NEXT(x, y)	((x << INDEX_SHIFT) | (y & INDEX_MASK))

#define DECODE_IDX(x)		(x & INDEX_MASK)
#define DECODE_CPU(x)		(x >> INDEX_SHIFT)

#define GET_NEXT_NODE(x, y)	(per_cpu(x, DECODE_CPU(y)) + DECODE_IDX(y))

typedef void* (*request_t)(void *);
typedef int (*test_thread_t)(void *);

int prepare_tests(test_thread_t, void *, const char *);
int test_thread(void *data);
int test_thread2(void *data);

struct cc_node {
	request_t req;
	void* params;
	void* ret;
	int next;
#ifdef DEBUG
	int prev __attribute__((aligned(L1_CACHE_BYTES)));
	atomic_t refcount;
#endif
	bool wait __attribute__((aligned(L1_CACHE_BYTES)));
	bool completed;
};

DEFINE_PER_CPU(struct cc_node, node_array[2]) = {
	{
		.req = NULL,
		.params = NULL,
		.ret = NULL,
		.wait = false,
		.completed = false,
		.next = ENCODE_NEXT(NR_CPUS, 1),
#ifdef DEBUG
		.prev = ENCODE_NEXT(NR_CPUS, 0),
		.refcount = ATOMIC_INIT(0),
#endif
	},
	{
		.req = NULL,
		.params = NULL,
		.ret = NULL,
		.wait = false,
		.completed = false,
		.next = ENCODE_NEXT(NR_CPUS, 0),
#ifdef DEBUG
		.prev = ENCODE_NEXT(NR_CPUS, 1),
		.refcount = ATOMIC_INIT(0),
#endif
	},
};

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

/* At first, lock is NULL value. This mean pointing the
 * CPU 0, idx 0. Thus, To make it consistent node_array_idx
 * should be set 1
 */
DEFINE_PER_CPU(int, node_array_idx) = 1;

#ifdef DEBUG
static int node_trace[MAX_CPU*2];
static int node_trace_idx = 0;
#endif
static int dump_cclock(void);

void* execute_cs(request_t req, void *params, atomic_t *lock)
{
	struct cc_node *prev, *pending;
	struct cc_node *next;
	int counter = 0;
	unsigned int this_cpu = get_cpu();
	unsigned int pending_cpu;
	unsigned int prev_cpu;
	request_t pending_req;

	/* get/update node_arra_idx */
	int this_cpu_idx = per_cpu(node_array_idx, this_cpu);
	per_cpu(node_array_idx, this_cpu) = this_cpu_idx? 0 : 1;

	this_cpu = ENCODE_NEXT(this_cpu, this_cpu_idx);
	next = GET_NEXT_NODE(node_array, this_cpu);
	WRITE_ONCE(next->req, NULL);
	WRITE_ONCE(next->params,NULL);
	WRITE_ONCE(next->ret, NULL);
	WRITE_ONCE(next->wait, true);
	WRITE_ONCE(next->completed, false);
	WRITE_ONCE(next->next, ENCODE_NEXT(NR_CPUS, 0));
	smp_wmb();

#ifdef DEBUG
	atomic_inc(&next->refcount);
#endif
	do {
		prev_cpu = READ_ONCE(lock->counter);
	} while(atomic_cmpxchg(lock, prev_cpu, this_cpu) != prev_cpu);
	WARN(prev_cpu == this_cpu, "lockbench: prev_cpu == this_cpu, Can't be happend!!!");
#ifdef DEBUG
	next->prev = prev_cpu;
#endif
	prev = GET_NEXT_NODE(node_array, prev_cpu);
	/* request and parameter should be set. Then, next should be set */
	WRITE_ONCE(prev->req, req);
	WRITE_ONCE(prev->params, params);
	smp_wmb();
	if (DECODE_CPU(prev->next)!=NR_CPUS) {
		pr_err("prev->next's value is not NR_CPUS!!!"
			   "prev_cpu (%d,%d) prev->next (%d,%d) this_cpu (%d,%d)",
			   DECODE_CPU(prev_cpu), DECODE_IDX(prev_cpu),
			   DECODE_CPU(prev->next), DECODE_IDX(prev->next),
			   DECODE_CPU(this_cpu), DECODE_IDX(this_cpu));
		dump_cclock();
	}
	WRITE_ONCE(prev->next, this_cpu);

	/* Failed to get lock */
	pr_debug("lockbench: prev{CPU: (%d, %d), wait:%d, completed:%d}\n"
					"lockbench: next{CPU: (%d, %d), wait:%d, completed:%d}\n",
					DECODE_CPU(prev_cpu), DECODE_IDX(prev_cpu),
					prev->wait, prev->completed,
					DECODE_CPU(this_cpu), DECODE_IDX(this_cpu),
					next->wait, next->completed);

	pr_debug("lockbench: Spinning start!\n");
	while (READ_ONCE(prev->wait))
		cpu_relax();

	smp_rmb();
	if (READ_ONCE(prev->completed)) {
#ifdef DEBUG
		atomic_dec(&next->refcount);
#endif
		put_cpu();
		pr_debug("lockbench: <Normal thread> CPU: (%d, %d) end of critical section!\n",
						DECODE_CPU(this_cpu), DECODE_IDX(this_cpu));
		return prev->ret;
	}

	/* Success to get lock */
	pending_cpu = prev_cpu;

	while (counter++ < MAX_COMBINER_OPERATIONS) {
		pending = GET_NEXT_NODE(node_array, pending_cpu);
#ifdef DEBUG
		node_trace[node_trace_idx] = pending_cpu;
		node_trace_idx = (node_trace_idx + 1) % (MAX_CPU*2);
#endif
		pending_cpu = READ_ONCE(pending->next);

		/* Keep ordering next -> (req, params)*/
		smp_rmb();
		if(!(READ_ONCE(pending->wait)) && (READ_ONCE(pending->completed))) {
			pr_err("Target node already done..."
				   "prev_cpu: (%d,%d), wait:%d, complete: %d "
				   "this_cpu: (%d,%d)",
				    DECODE_CPU(pending_cpu), DECODE_IDX(pending_cpu),
				READ_ONCE(pending->wait), READ_ONCE(pending->completed),
				DECODE_CPU(this_cpu), DECODE_IDX(this_cpu));
			dump_cclock();
		}

		/* Branch prediction: which case is more profitable? */
		if (DECODE_CPU(pending_cpu) == NR_CPUS)
			goto out;

		pr_debug("lockbench: CPU: (%d, %d), next_cpu: (%d, %d), request: %pF\n",
						DECODE_CPU(pending_cpu), DECODE_IDX(pending_cpu),
						DECODE_CPU(pending->next), DECODE_IDX(pending->next),
						pending->req);

		pending_req = READ_ONCE(pending->req);
		/* Preserve store order completed -> wait -> next */
		WARN(pending_req == NULL, "lockbench: pending->req == NULL...");
		WRITE_ONCE(pending->ret, pending_req(pending->params));
		WRITE_ONCE(pending->completed, true);
		smp_wmb();
		WRITE_ONCE(pending->wait, false);
	}
	/* Pass tho combiner thread role */
	WARN(DECODE_CPU(pending_cpu) == NR_CPUS, "lockbench: DECODE_CPU(pedning_cpu) == NR_CPUS...");
	pr_debug("lockbench: pass the combiner role to CPU: (%d, %d)\n",
					DECODE_CPU(pending_cpu), DECODE_IDX(pending_cpu));

	pending = GET_NEXT_NODE(node_array, pending_cpu);
out:
	smp_wmb();
	WRITE_ONCE(pending->wait, false);
#ifdef DEBUG
	atomic_dec(&next->refcount);
#endif
	put_cpu();
	pr_debug("lockbench: <Combiner thread> end of critical section!\n");
	return prev->ret;
}

/* Dummy workload */
DEFINE_SPINLOCK(dummy_spinlock);
atomic_t dummy_lock = ATOMIC_INIT(0);
int dummy_counter = 0;
int cache_table[128];
void* dummy_increment(void* params)
{
	int i;
	int *counter = (int*)params;
	if (unlikely(counter == NULL)) {
		printk("!!!! counter: %p", (counter));
	} else {
		(*counter)++;
	}

	for (i = 0; i < 128; i++)
		cache_table[i]++;

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

	if (val == 1) {
		for_each_online_cpu(cpu) {
			li = &per_cpu(lb_info_array, cpu);
			li->req = dummy_increment;
			li->params = &dummy_counter;
			li->lock = (void *)&dummy_lock;
			li->counter = nr_bench;
			li->monitor = monitor_thread;
			monitor_thread = false;
		}
		prepare_tests(test_thread, (void *)&lb_info_array, "cc-lockbench");
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
		prepare_tests(test_thread2, (void *)&lb_info_array, "spinlockbench");
	}

	(*ppos)++;
	return cnt;
}

static ssize_t lb_quit(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	unsigned long val;
	int ret, cpu;
	struct lb_info *ld;
	struct cc_node *node;
	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);

	if (ret)
		return ret;

	if (val == 1) {
		dummy_lock.counter = ENCODE_NEXT(0, 0);
		for_each_online_cpu(cpu) {
			ld = &per_cpu(lb_info_array, cpu);
			ld->quit = true;

			smp_mb();
			node = per_cpu(node_array, cpu);
			node[0].wait = false;
			node[0].completed = true;
			node[1].wait = false;
			node[1].completed = true;
		}
		per_cpu(node_array, 0)[0].completed = false;
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

	if (val > 0 && val < 15)
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

	if (val > 0 && val < NR_BENCH)
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
	int cpu, idx;
	struct cc_node *node;

	seq_printf(m, "<Node_array information>\n");
	seq_printf(m, "dummy_lock: (%d, %d)\n",
					DECODE_CPU(dummy_lock.counter), DECODE_IDX(dummy_lock.counter));
	for_each_online_cpu(cpu) {
		node = per_cpu(node_array, cpu);
		idx = per_cpu(node_array_idx, cpu);
		seq_printf(m, "Node idx: %d\n", idx);
		seq_printf(m, "Node(%d, %d) {\n"
						"\treq = %pF,\n"
						"\tparams = %p,\n"
						"\twait = %d, completed = %d,\n"
#ifdef DEBUG
						"\trefcount = %d,\n"
						"\tNext (%d, %d)\n"
						"\tPrev (%d, %d)\n}\n",
#else
						"\tNext (%d, %d)\n",
#endif
						cpu, 0,
						node[0].req, node[0].params,
						node[0].wait, node[0].completed,
#ifdef DEBUG
						node[0].refcount.counter,
						DECODE_CPU(node[0].next),
						DECODE_IDX(node[0].next),
						DECODE_CPU(node[0].prev),
						DECODE_IDX(node[0].prev)
#else
						DECODE_CPU(node[0].next),
						DECODE_IDX(node[0].next)
#endif
						);
		seq_printf(m, "Node(%d, %d) {\n"
						"\treq = %pF,\n"
						"\tparams = %p,\n"
						"\twait = %d, completed = %d,\n"
#ifdef DEBUG
						"\trefcount = %d,\n"
						"\tNext (%d, %d)\n"
						"\tPrev (%d, %d)\n}\n",
#else
						"\tNext (%d, %d)\n",
#endif
						cpu, 1,
						node[1].req, node[1].params,
						node[1].wait, node[1].completed,
#ifdef DEBUG
						node[1].refcount.counter,
						DECODE_CPU(node[1].next),
						DECODE_IDX(node[1].next),
						DECODE_CPU(node[1].prev),
						DECODE_IDX(node[1].prev)
#else
						DECODE_CPU(node[1].next),
						DECODE_IDX(node[1].next)
#endif
						);
	}

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

	return 0;
}

static int lb_debugfs_exit(void)
{
	debugfs_remove_recursive(lb_debugfs_root);
	return 0;
}

int test_thread(void *data)
{
	int i;
	int cpu = get_cpu();
	unsigned long flags;
	struct lb_info * lb_data = &per_cpu(*((struct lb_info *)data), cpu);
	unsigned long prev = 0, cur;

	if (unlikely(lb_data->monitor))
		prev = sched_clock();
	for (i=0; i<lb_data->counter && !READ_ONCE(lb_data->quit); i++) {
		if (unlikely(lb_data->monitor && i && ((i % 1000)==0))) {
			cur = sched_clock();
			printk("lockbench: <cc-lock> monitor thread %dth [%lu]\n", i, cur-prev);
			prev = cur;
		}
		local_irq_save(flags);
		preempt_disable();
		execute_cs(lb_data->req, lb_data->params, lb_data->lock);
		local_irq_restore(flags);
		preempt_enable();
	}

	if (unlikely(lb_data->monitor)) {
		cur = sched_clock();
		printk("lockbench: <cc-lock> monitor thread %dth [%lu]\n", i, cur-prev);
	}

	per_cpu(task_array, cpu) = NULL;
	put_cpu();
	return 0;
}

int test_thread2(void *data)
{
	int i;
	int cpu = get_cpu();
	struct lb_info * lb_data = &per_cpu(*((struct lb_info *)data), cpu);
	unsigned long prev = 0, cur = 0;
	unsigned long flags;
	spinlock_t *lock = (spinlock_t *)lb_data->lock;

	if (unlikely(lb_data->monitor))
		prev = sched_clock();
	for (i=0; i<lb_data->counter && !READ_ONCE(lb_data->quit); i++) {
		if (unlikely(lb_data->monitor && i && ((i % 1000)==0))) {
			cur = sched_clock();
			printk("lockbench: <spinlock> monitor thread %dth [%lu]\n", i, cur-prev);
			prev = cur;
		}
		spin_lock_irqsave(lock, flags);
		lb_data->req(lb_data->params);
		spin_unlock_irqrestore(lock, flags);
	}

	if (unlikely(lb_data->monitor)) {
		cur = sched_clock();
		printk("lockbench: <spinlock> monitor thread %dth [%lu]\n", i, cur-prev);
	}

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

static int dump_cclock(void)
{
	int cpu, idx;
	struct cc_node *node;
	struct lb_info *ld;
	int tmp;

	pr_err("<Node_array information>\n");
	pr_err("dummy_lock: (%d, %d)\n",
					DECODE_CPU(dummy_lock.counter), DECODE_IDX(dummy_lock.counter));
	pr_err("last used node:\n");
	for (idx=6; idx>0; idx--){
		tmp = node_trace[(node_trace_idx + (MAX_CPU*2) - (idx)) % (MAX_CPU*2)];
		pr_err("(%d,%d)->", DECODE_CPU(tmp), DECODE_IDX(tmp));
	}
	for_each_online_cpu(cpu) {
		node = per_cpu(node_array, cpu);
		idx = per_cpu(node_array_idx, cpu);
		pr_err("Node idx: %d\n", idx);
		pr_err("Node(%d, %d) {\n"
						"\treq = %pF,\n"
						"\tparams = %p,\n"
						"\twait = %d, completed = %d,\n"
#ifdef DEBUG
						"\trefcount = %d,\n"
						"\tNext (%d, %d)\n"
						"\tPrev (%d, %d)\n}\n",
#else
						"\tNext (%d, %d)\n",
#endif
						cpu, 0,
						node[0].req, node[0].params,
						node[0].wait, node[0].completed,
#ifdef DEBUG
						node[0].refcount.counter,
						DECODE_CPU(node[0].next),
						DECODE_IDX(node[0].next),
						DECODE_CPU(node[0].prev),
						DECODE_IDX(node[0].prev)
#else
						DECODE_CPU(node[0].next),
						DECODE_IDX(node[0].next)
#endif
						);
		pr_err("Node(%d, %d) {\n"
						"\treq = %pF,\n"
						"\tparams = %p,\n"
						"\twait = %d, completed = %d,\n"
#ifdef DEBUG
						"\trefcount = %d,\n"
						"\tNext (%d, %d)\n"
						"\tPrev (%d, %d)\n}\n",
#else
						"\tNext (%d, %d)\n",
#endif
						cpu, 1,
						node[1].req, node[1].params,
						node[1].wait, node[1].completed,
#ifdef DEBUG
						node[1].refcount.counter,
						DECODE_CPU(node[1].next),
						DECODE_IDX(node[1].next),
						DECODE_CPU(node[1].prev),
						DECODE_IDX(node[1].prev)
#else
						DECODE_CPU(node[1].next),
						DECODE_IDX(node[1].next)
#endif
						);
		ld = &per_cpu(lb_info_array, cpu);
		ld->quit = true;

		smp_mb();
		node[0].wait = false;
		node[0].completed = true;
		node[1].wait = false;
		node[1].completed = true;
	}
	dummy_lock.counter = ENCODE_NEXT(0, 0);
	per_cpu(node_array, 0)[0].completed = false;

	BUG();
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
	int cpu;
	struct lb_info *ld;
	struct cc_node *node;

	for_each_online_cpu(cpu) {
		ld = &per_cpu(lb_info_array, cpu);
		ld->quit = true;

		smp_mb();
		node = per_cpu(node_array, cpu);
		node[0].wait = false;
		node[0].completed = true;
		node[1].wait = false;
		node[1].completed = true;
	}
	lb_debugfs_exit();
}

module_init(lock_benchmark_init);
module_exit(lock_benchmark_exit);

