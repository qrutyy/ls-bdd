/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef MARKED_POINTERS_H
#define MARKED_POINTERS_H

// TAGS SPECIFIC

#define TAG_VALUE(v, tag) ((v) | tag)
#define IS_TAGGED(v, tag) ((v) & tag)

// removes the tag from the pointer
#define STRIP_TAG(v, tag) (((size_t)v) & ~tag)

// marks the node by switching the last bit to 1 and vice versa
#define MARK_NODE(x) TAG_VALUE((size_t)(x), 0x1)
// returns bool* if pointer is marked
#define HAS_MARK(x) (IS_TAGGED(((size_t)x), 0x1) == 0x1)

#endif
