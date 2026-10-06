// SPDX-License-Identifier: GPL-2.0-only
/*
 * Scalar XXH3-64, ported from xxHash by Yann Collet (BSD-2-Clause,
 * https://github.com/Cyan4973/xxHash). Output is bit-identical to upstream
 * XXH3_64bits(), which has been frozen since xxHash 0.8.0.
 */

#include <linux/bitops.h>
#include <linux/swab.h>
#include <linux/unaligned.h>

#include "utils/xxh3.h"

#define XXH_PRIME32_1 0x9E3779B1U
#define XXH_PRIME32_2 0x85EBCA77U
#define XXH_PRIME32_3 0xC2B2AE3DU

#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3 0x165667B19E3779F9ULL
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL

#define XXH_PRIME_MX1 0x165667919E3779F9ULL
#define XXH_PRIME_MX2 0x9FB21C651E98DF25ULL

#define XXH3_SECRET_SIZE 192
#define XXH3_SECRET_SIZE_MIN 136
#define XXH3_STRIPE_LEN 64
#define XXH3_SECRET_CONSUME_RATE 8
#define XXH3_ACC_NB (XXH3_STRIPE_LEN / sizeof(u64))
#define XXH3_SECRET_LASTACC_START 7
#define XXH3_SECRET_MERGEACCS_START 11

#define XXH3_MIDSIZE_MAX 240
#define XXH3_MIDSIZE_STARTOFFSET 3
#define XXH3_MIDSIZE_LASTOFFSET 17

static const u8 xxh3_secret[XXH3_SECRET_SIZE] __aligned(64) = {
	0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
	0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
	0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
	0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
	0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
	0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
	0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
	0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
	0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
	0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
	0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
	0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

static inline u64 xxh3_read64(const u8 *p)
{
	return get_unaligned_le64(p);
}

static inline u32 xxh3_read32(const u8 *p)
{
	return get_unaligned_le32(p);
}

/* Full 64x64->128 product, folded back to 64 bits by xoring the halves. */
static inline u64 xxh3_mul128_fold64(u64 lhs, u64 rhs)
{
#ifdef CONFIG_ARCH_SUPPORTS_INT128
	unsigned __int128 product = (unsigned __int128)lhs * rhs;

	return (u64)product ^ (u64)(product >> 64);
#else
	u64 lo_lo = (u64)(u32)lhs * (u32)rhs;
	u64 hi_lo = (lhs >> 32) * (u32)rhs;
	u64 lo_hi = (u64)(u32)lhs * (rhs >> 32);
	u64 hi_hi = (lhs >> 32) * (rhs >> 32);
	u64 cross = (lo_lo >> 32) + (u32)hi_lo + lo_hi;
	u64 upper = (hi_lo >> 32) + (cross >> 32) + hi_hi;
	u64 lower = (cross << 32) | (u32)lo_lo;

	return lower ^ upper;
#endif
}

static inline u64 xxh64_avalanche(u64 h)
{
	h ^= h >> 33;
	h *= XXH_PRIME64_2;
	h ^= h >> 29;
	h *= XXH_PRIME64_3;
	h ^= h >> 32;

	return h;
}

static inline u64 xxh3_avalanche(u64 h)
{
	h ^= h >> 37;
	h *= XXH_PRIME_MX1;
	h ^= h >> 32;

	return h;
}

static inline u64 xxh3_rrmxmx(u64 h, u64 len)
{
	h ^= rol64(h, 49) ^ rol64(h, 24);
	h *= XXH_PRIME_MX2;
	h ^= (h >> 35) + len;
	h *= XXH_PRIME_MX2;

	return h ^ (h >> 28);
}

static inline u64 xxh3_mix16b(const u8 *in, const u8 *secret)
{
	return xxh3_mul128_fold64(xxh3_read64(in) ^ xxh3_read64(secret),
				  xxh3_read64(in + 8) ^ xxh3_read64(secret + 8));
}

static u64 xxh3_len_1to3(const u8 *in, size_t len, const u8 *secret)
{
	u32 combined = ((u32)in[0] << 16) | ((u32)in[len >> 1] << 24) | ((u32)in[len - 1] << 0) | ((u32)len << 8);
	u64 bitflip = xxh3_read32(secret) ^ xxh3_read32(secret + 4);

	return xxh64_avalanche((u64)combined ^ bitflip);
}

static u64 xxh3_len_4to8(const u8 *in, size_t len, const u8 *secret)
{
	u64 bitflip = xxh3_read64(secret + 8) ^ xxh3_read64(secret + 16);
	u64 in64 = xxh3_read32(in + len - 4) + ((u64)xxh3_read32(in) << 32);

	return xxh3_rrmxmx(in64 ^ bitflip, len);
}

static u64 xxh3_len_9to16(const u8 *in, size_t len, const u8 *secret)
{
	u64 lo = xxh3_read64(in) ^ (xxh3_read64(secret + 24) ^ xxh3_read64(secret + 32));
	u64 hi = xxh3_read64(in + len - 8) ^ (xxh3_read64(secret + 40) ^ xxh3_read64(secret + 48));
	u64 acc = len + swab64(lo) + hi + xxh3_mul128_fold64(lo, hi);

	return xxh3_avalanche(acc);
}

static u64 xxh3_len_0to16(const u8 *in, size_t len, const u8 *secret)
{
	if (len > 8)
		return xxh3_len_9to16(in, len, secret);
	if (len >= 4)
		return xxh3_len_4to8(in, len, secret);
	if (len)
		return xxh3_len_1to3(in, len, secret);

	return xxh64_avalanche(xxh3_read64(secret + 56) ^ xxh3_read64(secret + 64));
}

static u64 xxh3_len_17to128(const u8 *in, size_t len, const u8 *secret)
{
	u64 acc = len * XXH_PRIME64_1;

	if (len > 32) {
		if (len > 64) {
			if (len > 96) {
				acc += xxh3_mix16b(in + 48, secret + 96);
				acc += xxh3_mix16b(in + len - 64, secret + 112);
			}
			acc += xxh3_mix16b(in + 32, secret + 64);
			acc += xxh3_mix16b(in + len - 48, secret + 80);
		}
		acc += xxh3_mix16b(in + 16, secret + 32);
		acc += xxh3_mix16b(in + len - 32, secret + 48);
	}
	acc += xxh3_mix16b(in, secret);
	acc += xxh3_mix16b(in + len - 16, secret + 16);

	return xxh3_avalanche(acc);
}

static u64 xxh3_len_129to240(const u8 *in, size_t len, const u8 *secret)
{
	u32 nr_rounds = len / 16;
	u64 acc = len * XXH_PRIME64_1;
	u64 acc_end;
	u32 i;

	for (i = 0; i < 8; i++)
		acc += xxh3_mix16b(in + 16 * i, secret + 16 * i);

	acc_end = xxh3_mix16b(in + len - 16, secret + XXH3_SECRET_SIZE_MIN - XXH3_MIDSIZE_LASTOFFSET);
	acc = xxh3_avalanche(acc);

	for (i = 8; i < nr_rounds; i++)
		acc_end += xxh3_mix16b(in + 16 * i, secret + 16 * (i - 8) + XXH3_MIDSIZE_STARTOFFSET);

	return xxh3_avalanche(acc + acc_end);
}

static inline void xxh3_accumulate_512(u64 *acc, const u8 *in, const u8 *secret)
{
	u32 i;

	for (i = 0; i < XXH3_ACC_NB; i++) {
		u64 data_val = xxh3_read64(in + 8 * i);
		u64 data_key = data_val ^ xxh3_read64(secret + 8 * i);

		acc[i ^ 1] += data_val;
		acc[i] += (u64)(u32)data_key * (data_key >> 32);
	}
}

static inline void xxh3_scramble(u64 *acc, const u8 *secret)
{
	u32 i;

	for (i = 0; i < XXH3_ACC_NB; i++) {
		u64 a = acc[i];

		a ^= a >> 47;
		a ^= xxh3_read64(secret + 8 * i);
		acc[i] = a * XXH_PRIME32_1;
	}
}

static void xxh3_accumulate(u64 *acc, const u8 *in, const u8 *secret, size_t nr_stripes)
{
	size_t n;

	for (n = 0; n < nr_stripes; n++)
		xxh3_accumulate_512(acc, in + n * XXH3_STRIPE_LEN, secret + n * XXH3_SECRET_CONSUME_RATE);
}

static u64 xxh3_merge_accs(const u64 *acc, const u8 *secret, u64 start)
{
	u64 result = start;
	u32 i;

	for (i = 0; i < 4; i++)
		result += xxh3_mul128_fold64(acc[2 * i] ^ xxh3_read64(secret + 16 * i),
					     acc[2 * i + 1] ^ xxh3_read64(secret + 16 * i + 8));

	return xxh3_avalanche(result);
}

static u64 xxh3_hash_long(const u8 *in, size_t len, const u8 *secret)
{
	u64 acc[XXH3_ACC_NB] = {
		XXH_PRIME32_3, XXH_PRIME64_1, XXH_PRIME64_2, XXH_PRIME64_3,
		XXH_PRIME64_4, XXH_PRIME32_2, XXH_PRIME64_5, XXH_PRIME32_1,
	};
	const size_t stripes_per_block = (XXH3_SECRET_SIZE - XXH3_STRIPE_LEN) / XXH3_SECRET_CONSUME_RATE;
	const size_t block_len = XXH3_STRIPE_LEN * stripes_per_block;
	const size_t nr_blocks = (len - 1) / block_len;
	size_t nr_stripes;
	size_t n;

	for (n = 0; n < nr_blocks; n++) {
		xxh3_accumulate(acc, in + n * block_len, secret, stripes_per_block);
		xxh3_scramble(acc, secret + XXH3_SECRET_SIZE - XXH3_STRIPE_LEN);
	}

	/* Last partial block, then the final stripe, which may overlap it. */
	nr_stripes = ((len - 1) - block_len * nr_blocks) / XXH3_STRIPE_LEN;
	xxh3_accumulate(acc, in + nr_blocks * block_len, secret, nr_stripes);
	xxh3_accumulate_512(acc, in + len - XXH3_STRIPE_LEN,
			    secret + XXH3_SECRET_SIZE - XXH3_STRIPE_LEN - XXH3_SECRET_LASTACC_START);

	return xxh3_merge_accs(acc, secret + XXH3_SECRET_MERGEACCS_START, (u64)len * XXH_PRIME64_1);
}

u64 lsv_xxh3_64(const void *data, size_t len)
{
	const u8 *in = data;

	if (len <= 16)
		return xxh3_len_0to16(in, len, xxh3_secret);
	if (len <= 128)
		return xxh3_len_17to128(in, len, xxh3_secret);
	if (len <= XXH3_MIDSIZE_MAX)
		return xxh3_len_129to240(in, len, xxh3_secret);

	return xxh3_hash_long(in, len, xxh3_secret);
}
