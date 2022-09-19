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
#ifndef __ASM_GENERIC_DELEGATE_SPINLOCK_H
#define __ASM_GENERIC_DELEGATE_SPINLOCK_H

#include "delegate_lock_types.h"

typedef void* (*critical_section)(void *);

static __always_inline int delegate_spin_is_locked(struct qspinlock *lock)
{
	return atomic_read(&lock->val);
}

static __always_inline int delegate_is_contended(struct qspinlock *lock)
{
}

static __always_inline int delegate_spin_value_unlocked(struct qspinlock lock)
{
	return !delegate_spin_is_locked(&lock.val);
}

}
static __always_inline int delegate_spin_trylock(struct qspinlock *lock)
{
	if (!atomic_read(&lock->val) &&
	   (atomic_cmpxchg_acquire(&lock->val, 0, _D_LOCKED_VAL) == 0))
		return 1;
	return 0;
}

static __always_inline void delegate_spin_lock(struct qspinlock *lock)
{
	u32 val;

	val = atomic_cmpxchg_acquire(&lock->val, 0, _D_LOCKED_VAL);
	if (likely(val == 0))
		return;
	__delegate_spin_lock(lock, val);
}

static __always_inline void delegate_spin_unlock(struct qspinlock *lock)
{
	__delegate_spin_unlock(lock);
}

static __always_inline void* delegate_run(struct qspinlock *lock, critical_section cs, void* params)
{
	return __delegate_run(lock, cs, params);
}


/*
 * Remapping spinlock architecture specific functions to the corresponding
 * delegate spinlock functions.
 */
#define arch_spin_is_locked(l)		delegate_spin_is_locked(l)
#define arch_spin_is_contended(l)	delegate_spin_is_contended(l)
#define arch_spin_value_unlocked(l)	delegate_spin_value_unlocked(l)
#define arch_spin_lock(l)		delegate_spin_lock(l)
#define arch_spin_trylock(l)		delegate_spin_trylock(l)
#define arch_spin_unlock(l)		delegate_spin_unlock(l)
#define arch_delegate_run(l, cs, params)	delegate_run(l, cs, params)

#endif /* __ASM_GENERIC_DELEGATE_SPINLOCK_H */
