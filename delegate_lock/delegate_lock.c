/*
 * Delegate spinlock
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Authors: Wonhyuk Yang <wonhyuk@postech.ac.kr>
 */


#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/mutex.h>
#include <linux/prefetch.h>
#include <asm/byteorder.h>
#include <linux/module.h>

#include "delegate_lock.h"
#include "delegate_mcs_spinlock.h"

#define MAX_NODES	4

MODULE_LICENSE("GPL");

/*
 * Per-CPU queue node structures; we can never have more than 4 nested
 * contexts: task, softirq, hardirq, nmi.
 *
 * Exactly fits one 64-byte cacheline on a 64-bit architecture.
 *
 */
static DEFINE_PER_CPU_ALIGNED(struct delegate_mcs_spinlock, delegate_mcs_nodes[MAX_NODES]);

static inline __pure u32 encode_tail(int cpu, int idx)
{
	u32 tail;

	tail  = (cpu + 1) << _D_WAITER_CPU_OFFSET;
	tail |= idx << _D_WAITER_IDX_OFFSET; /* assume < 4 */

	return tail;
}

static inline __pure struct delegate_mcs_spinlock *decode_tail(u32 tail)
{
	int cpu = (tail >> _D_WAITER_CPU_OFFSET) - 1;
	int idx = (tail &  _D_WAITER_IDX_MASK) >> _D_WAITER_IDX_OFFSET;

	return per_cpu_ptr(&delegate_mcs_nodes[idx], cpu);
}

u32 xchg_tail(struct delegate_spinlock *lock, u32 tail)
{
	u32 old;
	u32 new = _D_LOCKED_VAL | tail;
	for (;;) {
		/* FIXME: Need to be peek logic */
		pr_debug("[REGIST 1] cpu: %x lock: %x, cond: %x, new: %x", smp_processor_id(), atomic_read(&lock->val), _D_LOCKED_VAL, new);
		old = atomic_cmpxchg_acquire(&lock->val, _D_LOCKED_VAL, new);
		pr_debug("[REGIST 2] cpu: %x lock: %x, cond: %x, new: %x", smp_processor_id(), atomic_read(&lock->val), old, new);
		if (old == _D_LOCKED_VAL)
			break;
		else if (old & _D_WAITER_MASK)
			break;
		else if (old == 0 && delegate_spin_trylock(lock))
			break;
	}
	return old;
}

void clear_waiter(struct delegate_spinlock *lock)
{
	atomic_set(&lock->val, _D_LOCKED_VAL);
}

void *delegate_put_node(void)
{
	struct delegate_mcs_spinlock *node;
	int idx;
	void *ret;
	
	node = this_cpu_ptr(&delegate_mcs_nodes[0]);
	idx = node->count;

	node += idx - 1;
	ret = node->ret;

	node->count--;
	return ret;
}

int delegate_busy_waiting(struct delegate_spinlock *lock, critical_section cs, void *params)
{
	/*
	 * Busy waiting phase
	 * 
	 * Return 0: Delicated job done
	 * Return 1: Selected as a delegate thread
	 */
	struct delegate_mcs_spinlock *node;
	u32 old, tail;
	int idx;
	
	node = this_cpu_ptr(&delegate_mcs_nodes[0]);
	idx = node->count++;
	node += idx;

	tail = encode_tail(smp_processor_id(), idx);

	/*
	 * Ensure that we increment the head node->count before initialising
	 * the actual node. If the compiler is kind enough to reorder these
	 * stores, then an IRQ could overwrite our assignments.
	 */
	barrier();

	node->lock = lock;
	node->cs = cs;
	node->params = params;

	smp_wmb();
	node->delegate = 0;
	node->locked = 0;
	smp_wmb();

	/* 
	 * old == 0: Acquired lock(Run as delegate thread)
	 * old != 0: Busy waiting until unlocked
	 */
	old = xchg_tail(lock, tail);
	if (!old)
		return 1;	

	if (!node->cs || old == 1) {
		arch_mcs_spin_lock_contended(&node->delegate);
		return 1;
	}
	else {
		arch_mcs_spin_lock_contended(&node->locked);
		return 0;
	}
	
}

void delegate_execution(struct delegate_spinlock *lock)
{
	struct delegate_mcs_spinlock *next;
	u32 old, tail;
	int idx, cpu;
	
	for_each_cpu_wrap(cpu, cpu_online_mask, smp_processor_id()) {
		next = per_cpu_ptr(delegate_mcs_nodes, cpu);
		for (idx = next->count-1; idx >= 0;idx--) {
			if (next[idx].lock != lock || next[idx].locked)
				continue;
			if (!next->cs)
				goto yield;
			next[idx].ret = next[idx].cs(next[idx].params);
			arch_mcs_spin_unlock_contended(&next[idx].locked);
		}
	}

	pr_debug("[UNLOCK 1] cpu: %x lock: %x, cond: %x, new: %x", smp_processor_id(), atomic_read(&lock->val), _D_LOCKED_VAL, 0);
	old = atomic_cmpxchg_relaxed(&lock->val, _D_LOCKED_VAL, 0);
	pr_debug("[UNLOCK 2] cpu: %x lock: %x, cond: %x, new: %x", smp_processor_id(), atomic_read(&lock->val), old, 0);
	if (old != _D_LOCKED_VAL) {
		tail = old & _D_WAITER_MASK;
		next = decode_tail(tail);
		goto yield;
	}
	return;
yield:
	/* clear waiter */
	clear_waiter(lock);
	arch_mcs_spin_unlock_contended(&next->delegate);
	
	return;
}

void __delegate_spin_lock(struct delegate_spinlock *lock, u32 val)
{
	delegate_busy_waiting(lock, NULL, NULL);
}

void __delegate_spin_unlock(struct delegate_spinlock *lock)
{
	/* Run as delegate thread*/
	delegate_execution(lock);
	delegate_put_node();
}

void* __delegate_run(struct delegate_spinlock *lock, critical_section cs, void* params)
{
	int ret;	
	//pr_debug("cpu %d: lock: %x busy-waiting-phase", lock->val, smp_processor_id());
	ret = delegate_busy_waiting(lock, cs, params);
	if (!ret)
		return delegate_put_node();
	
	//pr_debug("cpu %d: lock: %x Delegate-phase", lock->val, smp_processor_id());
	/* Run as delegate thread*/
	delegate_execution(lock);
	return delegate_put_node();
}

