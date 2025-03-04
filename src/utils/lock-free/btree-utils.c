// SPDX-License-Identifier: GPL-2.0-only

#include <linux/btree.h>
#include <linux/module.h>
#include <linux/cache.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "btree-utils.h"

struct btree_geo {
	s32 keylen;
	s32 no_pairs;
	s32 no_longs;
};

static s32 longcmp(const unsigned long *l1, const unsigned long *l2, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (l1[i] < l2[i])
			return -1;
		if (l1[i] > l2[i])
			return 1;
	}
	return 0;
}


static unsigned long *longcpy(unsigned long *dest, const unsigned long *src,
		size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		dest[i] = src[i];
	return dest;
}

static unsigned long *bkey(struct btree_geo *geo, unsigned long *node, s32 n)
{
	return &node[n * geo->keylen];
}


static s32 keycmp(struct btree_geo *geo, unsigned long *node, int pos,
		  unsigned long *key)
{
	return longcmp(bkey(geo, node, pos), key, geo->keylen);
}

static void dec_key(struct btree_geo *geo, unsigned long *key)
{
	unsigned long val;
	s32 i;

	for (i = geo->keylen - 1; i >= 0; i--) {
		val = key[i];
		key[i] = val - 1;
		if (val)
			break;
	}
}

static s32 keyzero(struct btree_geo *geo, unsigned long *key)
{
	s32 i;

	for (i = 0; i < geo->keylen; i++)
		if (key[i])
			return 0;

	return 1;
}

static void *bval(struct btree_geo *geo, unsigned long *node, s32 n)
{
	return (void *)node[geo->no_longs + n];
}

void *btree_last_no_rep(struct btree_head *head, struct btree_geo *geo,
		 unsigned long *key)
{
	s32 height = head->height;
	unsigned long *node = head->node;

	if (height == 0)
		return NULL;

	for ( ; height > 1; height--)
		node = bval(geo, node, 0);

	return bval(geo, node, 0);
}

void *btree_get_next(struct btree_head *head, struct btree_geo *geo,
		     unsigned long *key)
{
	s32 i, height;
	unsigned long *node, *oldnode;
	unsigned long *retry_key = NULL;

	if (keyzero(geo, key))
		return NULL;

	if (head->height == 0)
		return NULL;
retry:
	dec_key(geo, key);

	node = head->node;
	for (height = head->height ; height > 1; height--) {
		for (i = geo->no_pairs; i > 0; i--) {
			if (keycmp(geo, node, i, key) >= 0)
				break;
		}
		if (i == geo->no_pairs)
			goto miss;
		oldnode = node;
		node = bval(geo, node, i);
		if (!node)
			goto miss;
		retry_key = bkey(geo, oldnode, i);
	}

	if (!node)
		goto miss;

	for (i = geo->no_pairs; i > 0; i--) {
		if (keycmp(geo, node, i, key) >= 0) {
			if (bval(geo, node, i)) {
				longcpy(key, bkey(geo, node, i), geo->keylen);
				return bval(geo, node, i);
			}
			goto miss;
		}
	}
miss:
	if (retry_key) {
		longcpy(key, retry_key, geo->keylen);
		retry_key = NULL;
		goto retry;
	}
	return NULL;
}

void *btree_get_prev_no_rep(struct btree_head *head, struct btree_geo *geo,
		     unsigned long *key, unsigned long *prev_key)
{
	s32 i, height;
	unsigned long *node, *oldnode;
	unsigned long *retry_key = NULL;

	if (keyzero(geo, key))
		return NULL;

	if (head->height == 0)
		return NULL;

	node = head->node;
	for (height = head->height ; height > 1; height--) {
		for (i = 0; i < geo->no_pairs; i++)
			if (keycmp(geo, node, i, key) <= 0)
				break;
		if (i == geo->no_pairs)
			goto miss;
		oldnode = node;
		node = bval(geo, node, i);
		if (!node)
			goto miss;
		retry_key = bkey(geo, oldnode, i);
	}

	if (!node)
		goto miss;

	for (i = 0; i < geo->no_pairs; i++) {
		if (keycmp(geo, node, i, key) <= 0) {
			if (bval(geo, node, i)) {
				*prev_key = *bkey(geo, node, i);
				pr_debug("B+Tree: prev_key %lu\n", *prev_key);
				return bval(geo, node, i);
			}
			goto miss;
		}
	}
miss:
	if (retry_key) {
		longcpy(key, retry_key, geo->keylen);
		retry_key = NULL;
		pr_err("B+Tree: get_prev: key miss\n");
	}
	prev_key = NULL;
	return NULL;
}
