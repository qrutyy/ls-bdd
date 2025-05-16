// SPDX-License-Identifier: GPL-2.0-only

#ifndef ATOMIC_OPS_H
#define ATOMIC_OPS_H

/**
 * Can be set as gcc functions like __sync_val_compare_and_swap,
 * both variants support atomicity so basically it doesn't matter (atomic-gcc.h even has a macro that makes it equal)
 */
#define SYNC_LCAS(ptr, old, new) cmpxchg64(ptr, old, (__typeof__(*ptr))new)
#define SYNC_LSWAP(ptr, val) xchg(ptr, val)

/**
 * atomic_* versions of cmpxchg or other actions are used for better cross-architecture work.
 * Btw, they provide more memory order guarantees and type safety.
 */
#define ATOMIC_LREAD(ptr) atomic64_read(ptr)
#define ATOMIC_INC(ptr) atomic64_inc(ptr)
#define ATOMIC_FAI(ptr) atomic64_fetch_add(1, ptr)
#define ATOMIC_FAD(ptr) atomic64_fetch_sub(1, ptr)
#define ATOMIC_LCAS(ptr, old, new) atomic64_cmpxchg(ptr, old, new)
#define ATOMIC_LSWAP(ptr, val) atomic64_xchg(ptr, val)

#endif
