/* SPDX-License-Identifier: GPL-2.0-only
 *
 * OPTIK lock: a lock that carries a version number, so that locking and
 * validating an optimistic traversal take one compare-and-swap.
 *
 * Guerraoui R., Trigonakis V. Optimistic Concurrency with OPTIK. PPoPP 2016.
 * This is the "versioned lock" implementation of the paper (its Figure 4):
 * the lock is one 64-bit counter, an odd value means locked, every critical
 * section that releases the lock with optik_unlock() adds two to it.
 *
 * The pattern, for a structure protected by one such lock:
 *   v = optik_get_version_wait(&lock);       // free version, taken first
 *   ... traverse and prepare, no locks ...
 *   if (!optik_trylock_version(&lock, v))    // someone changed it: start over
 *           goto restart;
 *   ... change the structure ...
 *   optik_unlock(&lock);                     // version += 2
 * An operation that finds nothing to change (a lookup, a removal of an absent
 * key) never takes the lock.
 *
 * A holder must not be preempted while waiters spin on the word, so the lock
 * is a preemption-disabled section, like a spinlock.
 */

#ifndef OPTIK_H
#define OPTIK_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/preempt.h>
#include <linux/atomic.h>
#include <asm/processor.h>

typedef u64 optik_t;

#define OPTIK_INIT 0
#define OPTIK_LOCKED 0x1ULL

static inline bool optik_is_locked(optik_t version)
{
	return version & OPTIK_LOCKED;
}

static inline bool optik_is_same_version(optik_t v0, optik_t v1)
{
	return v0 == v1;
}

/* The current version, possibly a locked one. Orders the reads after it. */
static inline optik_t optik_get_version(optik_t *lock)
{
	return smp_load_acquire(lock);
}

/* Waits for the lock to be free and returns its free version. */
static inline optik_t optik_get_version_wait(optik_t *lock)
{
	optik_t version;

	while (optik_is_locked(version = optik_get_version(lock)))
		cpu_relax();

	return version;
}

/*
 * Takes the lock if it is free and its version is still |target|.
 * Does not wait.
 */
static inline bool optik_trylock_version(optik_t *lock, optik_t target)
{
	optik_t expected = target;

	if (optik_is_locked(target) || READ_ONCE(*lock) != target)
		return false;

	preempt_disable();
	if (!try_cmpxchg(lock, &expected, target + 1)) {
		preempt_enable();
		return false;
	}

	return true;
}

/* Releases the lock and moves the version on: the structure was changed. */
static inline void optik_unlock(optik_t *lock)
{
	smp_store_release(lock, READ_ONCE(*lock) + 1);
	preempt_enable();
}

/* Releases the lock and keeps the version: the structure was not changed. */
static inline void optik_revert(optik_t *lock)
{
	smp_store_release(lock, READ_ONCE(*lock) - 1);
	preempt_enable();
}

#endif
