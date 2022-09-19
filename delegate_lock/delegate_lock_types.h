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
#ifndef __ASM_GENERIC_DELEGATE_SPINLOCK_TYPES_H
#define __ASM_GENERIC_DELEGATE_SPINLOCK_TYPES_H

typedef struct delegate_spinlock {
	union {
		atomic_t val;

#ifdef __LITTLE_ENDIAN
		struct {
			u8	locked;
			u8	reserved;
			u16	waiter;
		};
#else
		struct {
			u16	waiter;
			u8	reserved;
			u8	locked;
		};
#endif
	};
} my_arch_spinlock_t;

/*
 * Initializier
 */
#define	__ARCH_SPIN_LOCK_UNLOCKED	{ { .val = ATOMIC_INIT(0) } }

/*
 * Bitfields in the atomic value:
 *
 *  0- 7: locked byte
 *  8-15: not used
 * 16-17: first waiter cpu index
 * 18-31: first waiter cpu (+1)
 *
 */
#define	_D_SET_MASK(type)	(((1U << _D_ ## type ## _BITS) - 1)\
				      << _D_ ## type ## _OFFSET)
#define _D_LOCKED_OFFSET	0
#define _D_LOCKED_BITS		8
#define _D_LOCKED_MASK		_D_SET_MASK(LOCKED)

#define _D_WAITER_IDX_OFFSET	(_D_LOCKED_OFFSET + _D_LOCKED_BITS)
#define _D_WAITER_IDX_BITS	2
#define _D_WAITER_IDX_MASK	_D_SET_MASK(WAITER_IDX)

#define _D_WAITER_CPU_OFFSET	(_D_WAITER_IDX_OFFSET + _D_WAITER_IDX_BITS)
#define _D_WAITER_CPU_BITS	(32 - _D_WAITER_CPU_OFFSET)
#define _D_WAITER_CPU_MASK	_D_SET_MASK(WAITER_CPU)
#define _D_WAITER_MASK		(_D_WAITER_IDX_MASK | _D_WAITER_CPU_MASK)

#define _D_LOCKED_VAL		(1U << _D_LOCKED_OFFSET)
#endif /* __ASM_GENERIC_DELEGATE_SPINLOCK_TYPES_H */
