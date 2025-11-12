# Data Structure Micro-Standard

This document defines the minimal interface and conventions required for integrating new data structures into the LS-BDD framework.


## Prerequisites

1. **Memory management:**
   It is recommended to use **`kmem_cache`** for node allocation.
   Create a cache (e.g., `node_cache`) in your `ds_init` function and assign it to the appropriate field in `cache_mng->?_cache`.

2. **Header definitions:**
   Extend the following structures in `ds_control.h`:

   * `lsbdd_ds_type`
   * `lsbdd_ds`
   * `lsbdd_cache_mng`



## Interface Definition

Notation:
| Symbol           | Description                                       |
| ---------------- | ------------------------------------------------- |
| **id**           | Unique identifier (prefix) of your data structure |
| **ds_type**      | Type definition of the data structure itself      |
| **ds_node_type** | Type definition of the data structure node        |

The value type used for redirection is defined as:

```c
struct lsbdd_value_redir {
    sector_t redirected_sector;
    u32 block_size;
};
```

### Required API

| Function                                                                                                                            | Description                                                                                                 |
| ----------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `struct ds_type *id_init(struct kmem_cache *node_cache)`                                                                            | Allocates and initializes the data structure. Returns a pointer to the structure.                           |
| `void id_free(struct ds_type *ds, struct kmem_cache *node_cache, struct kmem_cache *lsbdd_value_cache)`                             | Deallocates the data structure and its contents.                                                            |
| `struct ds_node_type *id_lookup(struct ds_type *ds, sector_t key)`                                                                  | Looks up a node by key.                                                                                     |
| `void id_remove(struct ds_type *ds, sector_t key, struct kmem_cache *lsbdd_value_cache)`                                            | Removes the node with the specified key and frees its value.                                                |
| `s32 id_insert(struct ds_type *ds, sector_t key, void *value, struct kmem_cache *node_cache, struct kmem_cache *lsbdd_value_cache)` | Inserts a new key–value pair. Returns 0 on success or an error code otherwise.                              |
| `void *id_prev(struct ds_type *ds, sector_t key, sector_t *prev_key)`                                                               | Retrieves the node preceding the given key and stores its key in `prev_key`. Returns a pointer to the node. |
| `struct ds_node_type *id_last(struct ds_type *ds)`                                                                                  | Returns the last node in the data structure.                                                                |
| `bool id_empty_check(struct ds_type *ds)`                                                                                            | Returns true if the data structure is empty, otherwise false.                                                      |

**Examples:**
See [`lock-free/skiplist.c`](../src/lock-free/skiplist.c) or [`lock-free/hashtable.c`](../src/lock-free/hashtable.c) for reference implementations.


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

Some data structures are still **work-in-progress (WIP)** as they are being adapted to use `kmem_cache`-based memory management.
All existing implementations—except for `lock-free/rb-tree` and `lock-free/btree`—are algorithmically correct and fully functional.
