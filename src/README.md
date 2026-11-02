# Data Structure Micro-Standard

This document defines the minimal interface and conventions required for integrating new data structures into the LS-BDD framework.


## Registration

An index is a `struct lsv_ds_ops` (see `utils/ds_control.h`) appended to
`lsv_ds_registry[]` in `utils/ds_control.c`. Nothing else in the core refers to a
particular index: `lsv_ds_init()` finds it by name, and every other `lsv_ds_*`
call goes through `ds->ops`.

The index keeps its own state in the `structure` union of `struct lsv_ds`; extend
the union with a pointer to your type. Node caches, if any, are created lazily in
`init` into `struct lsv_cache_mng`, and are shared by every device.

## Interface

| Operation | Contract |
| --------- | -------- |
| `s32 init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)` | Allocates an empty index. `max_entries` bounds indexes that need a size up front. |
| `void free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)` | Frees the index; values still mapped are freed too with `LSV_DS_FREE_OWNS_VALUES`. |
| `void *lookup(struct lsv_ds *ds, sector_t key)` | The value of `key`, NULL if it is not mapped. |
| `s32 insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)` | Maps a key that is not mapped yet; 0 on success. |
| `void remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep)` | Unmaps `key`; frees its value with `LSV_DS_REMOVE_OWNS_VALUE`. |
| `sector_t last(struct lsv_ds *ds, sector_t key)` | The largest key, with `LSV_DS_ORDERED`. |
| `void *prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)` | The predecessor of `key`, with `LSV_DS_ORDERED`. |
| `bool empty(struct lsv_ds *ds)` | Whether no key is mapped. |

Keys are never the reserved 0 and `U64_MAX` (`lsv_ds_key_is_reserved()`): the
mapping layer stores logical cell `n` under key `n + 1`. Inserting a key that is
already mapped is outside the contract.

### Capabilities

`caps` declares how an index differs from the others, and the KUnit contract
checks each index against its own declaration:

| Flag | Meaning |
| ---- | ------- |
| `LSV_DS_FREE_OWNS_VALUES` | `free` releases the values still mapped |
| `LSV_DS_REMOVE_OWNS_VALUE` | `remove` releases the removed value |
| `LSV_DS_ORDERED` | `last` and `prev` follow key order |
| `LSV_DS_REMOVE_ABSENT_OK` | removing a key that is not mapped is a no-op |

## Tests

The `lsv_ds` suite (`utils/tests/kunit_lsv_ds.c`) is parameterised over the registry:

* `ds_registry` checks the registry itself; per index, `ds_starts_empty`,
  `ds_insert_lookup` (keys spread over 40 bits), `ds_lookup_absent`,
  `ds_wide_keys` (up to 2^47) and `ds_remove` check the contract.
* `ds_bench`, a slow case, reports single-thread ns/op of insert, lookup hit and
  lookup miss over 65536 sequential and spread keys, as `lsv_ds_bench ds=<name> ...`
  lines in the KUnit log.

A new index needs no test code: registering it is enough. In lock-free builds
every index operation of the suites runs on an `lsv_wq` worker, which the DtA
hashtable requires of its callers.

## Atomics and Primitives

### Marked Pointers (utils/lock-free/marked_pointers.h)

```c
/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef MARKED_POINTERS_H
#define MARKED_POINTERS_H

/* Tag manipulation */
#define TAG_VALUE(v, tag) ((v) | (tag))
#define IS_TAGGED(v, tag) ((v) & (tag))
#define STRIP_TAG(v, tag) (((size_t)(v)) & ~(tag))

/* Mark/unmark helpers */
#define MARK_NODE(x) TAG_VALUE((size_t)(x), 0x1)
#define HAS_MARK(x) (IS_TAGGED((size_t)(x), 0x1) == 0x1)

#endif /* MARKED_POINTERS_H */
```

### Atomic Operations (utils/lock-free/atomic_ops.h)

```c
/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ATOMIC_OPS_H
#define ATOMIC_OPS_H

/*
 * Can be defined using GCC built-ins like __sync_val_compare_and_swap.
 * Both variants guarantee atomicity; 'atomic-gcc.h' provides macros for compatibility.
 */
#define SYNC_LCAS(ptr, old, new) cmpxchg64(ptr, old, (__typeof__(*ptr))(new))
#define SYNC_LSWAP(ptr, val) xchg(ptr, val)

/*
 * atomic_* versions of cmpxchg and related operations provide
 * cross-architecture portability, memory ordering guarantees,
 * and type safety.
 */
#define ATOMIC_LREAD(ptr) atomic64_read(ptr)
#define ATOMIC_INC(ptr) atomic64_inc(ptr)
#define ATOMIC_FAI(ptr) atomic64_fetch_add(1, ptr)
#define ATOMIC_FAD(ptr) atomic64_fetch_sub(1, ptr)
#define ATOMIC_LCAS(ptr, old, new) atomic64_cmpxchg(ptr, old, new)
#define ATOMIC_LSWAP(ptr, val) atomic64_xchg(ptr, val)

#endif /* ATOMIC_OPS_H */
```

## Notes

The lock-free `rb` and `bt` are the kernel's rbtree and btree without
synchronisation: correct single-threaded, not under concurrent writers.
