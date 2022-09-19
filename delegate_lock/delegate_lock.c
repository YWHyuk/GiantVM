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

#include "delegate_lock.h"
#include "delegate_mcs_spinlock.h"

#define MAX_NODES	4

/*
 * Per-CPU queue node structures; we can never have more than 4 nested
 * contexts: task, softirq, hardirq, nmi.
 *
 * Exactly fits one 64-byte cacheline on a 64-bit architecture.
 *
 */
static DEFINE_PER_CPU_ALIGNED(struct delegate_mcs_spinlock, delgate_mcs_nodes[MAX_NODES]);

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

static __always_inline u32 xchg_tail(struct qspinlock *lock, u32 tail)
{
	u32 old;
	u32 new = _D_LOCKED_VAL | tail;
	for (;;) {
		old = atomic_cmpxchg_relaxed(&lock->val, _D_LOCKED_VAL, new);
		if (old == _D_LOCKED_VAL)
			break;
		else if (old & _D_WAITER_MASK)
			break;
		else if (old == 0 && delegate_spin_trylock(lock)) {
			break;
		}
	}
	return old;
}

static __always_inline void clear_waiter(struct delegate_spinlock *lock)
{
	WRITE_ONCE(lock->locked, _D_LOCKED_VAL);
}

static __always_inline void* delegate_put_node()
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

void __delegate_spin_lock(struct delegate_spinlock *lock, u32 val)
{
	delegate_busy_waiting(lock, NULL, NULL);
}
EXPORT_SYMBOL(__delegate_spin_lock);

void __delegate_spin_unlock(struct delegate_spinlock *lock)
{
	/* Run as delegate thread*/
	delegate_execution(lock);
	return delegate_put_node();
}
EXPORT_SYMBOL(__delegate_spin_unlock);

int delegate_busy_waiting(struct delegate_spinlock *lock, critical_section sc, void *params)
{
	/*
	 * Busy waiting phase
	 * 
	 * Return 0: Delicated job done
	 * Return 1: Selected as a delegate thread
	 */
	struct delegate_mcs_spinlock *next, *node;
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

	if (!node->cs || old & _D_WAITER_MASK != tail) {
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
	
	/* clear waiter */
	clear_waiter(lock);
	
	for_each_cpu_wrap(cpu, cpu_online_mask, smp_processor_id()) {
		next = per_cpu_ptr(delegate_mcs_nodes, cpu);
		for (idx = next->count-1; idx >= 0;) {
			if (next[idx].lock != lock || !next[idx].locked)
				continue;
			if (!next->cs)
				goto yield;
			next[idx].ret = next[idx].cs(next[idx].params);
			arch_mcs_spin_unlock_contended(&next[idx].locked);
		}
	}

	old = atomic_cmpxchg_relaxed(&lock->val, _D_LOCKED_VAL, 0);
	if (old != _D_LOCKED_VAL) {
		tail = old & _D_WAITER_MASK;
		next = decode_tail(tail);
		goto yield;
	}
	return;
yield:
	arch_mcs_spin_unlock_contended(&next->delegate);
	return;
}

void* __delegate_run(struct delegate_spinlock *lock, critical_section cs, void* params)
{
	int ret;	

	ret = delegate_busy_waiting(lock, cs, params);
	if (!ret)
		return delegate_put_node();
	
	/* Run as delegate thread*/
	delegate_execution(lock);
	return delegate_put_node();
}
EXPORT_SYMBOL(__delegate_run);
