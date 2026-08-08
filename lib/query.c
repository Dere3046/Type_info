// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/stddef.h>

#include "btf.h"
#include "lib.h"
#include "type_info.h"
#include "reg.h"

static const struct ti_ctx *q_ctx;

static const struct btf_type *ctx_type(const struct ti_ctx *c, u32 id)
{
	if (!id)
		return NULL;
	if (id <= c->cur.id_off) {
		if (!c->base)
			return NULL;
		return ctx_type(c->base, id);
	}
	return btf_type(&c->cur, id);
}

static const struct ti_ctx *ctx_base_of(const struct ti_ctx *c)
{
	while (c->base)
		c = c->base;
	return c;
}

static const char *ctx_str(const struct ti_ctx *c, u32 off)
{
	const struct ti_ctx *b;
	const char *s;

	if (off < c->cur.str_base_off) {
		b = ctx_base_of(c);
		if (off >= b->cur.str_len)
			return "";
		s = btf_str(&b->cur, off);
	} else {
		s = btf_str(&c->cur, off);
	}
	return s ? s : "";
}

static int ns_cmp(const void *a, const void *b)
{
	const struct ti_nsent *x = a;
	const struct ti_nsent *y = b;

	return strcmp(ctx_str(q_ctx, x->name_off), ctx_str(q_ctx, y->name_off));
}

static int ns_key_cmp(const void *key, const void *elt)
{
	const char *name = key;
	const struct ti_nsent *e = elt;

	return strcmp(name, ctx_str(q_ctx, e->name_off));
}

static void ns_swap(struct ti_nsent *x, struct ti_nsent *y)
{
	struct ti_nsent t = *x;

	*x = *y;
	*y = t;
}

static void ns_sift(struct ti_nsent *a, u32 n, u32 root)
{
	for (;;) {
		u32 l = root * 2 + 1;
		u32 r = l + 1;
		u32 m = root;

		if (l < n && ns_cmp(&a[l], &a[m]) > 0)
			m = l;
		if (r < n && ns_cmp(&a[r], &a[m]) > 0)
			m = r;
		if (m == root)
			break;
		ns_swap(&a[root], &a[m]);
		root = m;
	}
}

static void ns_sort(struct ti_nsent *a, u32 n)
{
	u32 i;

	for (i = n / 2; i-- > 0;)
		ns_sift(a, n, i);
	for (i = n - 1; i > 0; i--) {
		ns_swap(&a[0], &a[i]);
		ns_sift(a, i, 0);
	}
}

static struct ti_nsent *ns_bsearch(const char *name, struct ti_nsent *a,
				   u32 n)
{
	u32 lo = 0;
	u32 hi = n;

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		int c = ns_key_cmp(name, &a[mid]);

		if (!c)
			return &a[mid];
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

int ti_ctx_init(struct ti_ctx *c, const void *blob, u32 size)
{
	struct ti_nsent *map;
	u32 i;
	int ret;

	if (!c)
		return -EINVAL;

	ret = btf_open(blob, size, &c->cur);
	if (ret)
		return ret;

	if (c->base) {
		c->cur.id_off = c->base->cur.type_cnt;
		c->cur.str_base_off = c->base->cur.str_len;
	}

	map = vmalloc(sizeof(*map) * c->cur.type_cnt);
	if (!map) {
		btf_close(&c->cur);
		return -ENOMEM;
	}
	for (i = 0; i < c->cur.type_cnt; i++) {
		const struct btf_type *t;

		t = btf_type(&c->cur, c->cur.id_off + 1 + i);
		map[i].name_off = t->name_off;
		map[i].id = c->cur.id_off + 1 + i;
	}

	q_ctx = c;
	ns_sort(map, c->cur.type_cnt);
	q_ctx = NULL;

	c->name_map = map;
	c->name_cnt = c->cur.type_cnt;
	return 0;
}

int ti_ctx_open(const void *blob, u32 size, struct ti_ctx **out)
{
	struct ti_ctx *c;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	ret = ti_ctx_init(c, blob, size);
	if (ret) {
		kfree(c);
		return ret;
	}
	*out = c;
	return 0;
}

void ti_ctx_close(struct ti_ctx *ctx)
{
	if (!ctx)
		return;
	vfree(ctx->name_map);
	btf_close(&ctx->cur);
	kfree(ctx);
}

int ti_type_by_name(const struct ti_ctx *ctx, const char *name,
		    u32 kind_mask, u32 *out)
{
	struct ti_nsent *hit;
	u32 i;

	if (!ctx || !name || !out)
		return -EINVAL;

	q_ctx = ctx;
	hit = ns_bsearch(name, ctx->name_map, ctx->name_cnt);
	q_ctx = NULL;
	if (!hit) {
		int idx;

		if (kind_mask && !(kind_mask & BIT(BTF_KIND_STRUCT)))
			return -ENOENT;
		idx = ti_reg_lookup(name);
		if (idx < 0)
			return -ENOENT;
		*out = ti_reg_idx_to_id(idx);
		return 0;
	}

	i = hit - ctx->name_map;
	while (i > 0 &&
	       !strcmp(name, ctx_str(ctx, ctx->name_map[i - 1].name_off)))
		i--;
	for (; i < ctx->name_cnt &&
	       !strcmp(name, ctx_str(ctx, ctx->name_map[i].name_off)); i++) {
		const struct btf_type *t;

		t = btf_type(&ctx->cur, ctx->name_map[i].id);
		if (kind_mask && !(kind_mask & BIT(BTF_INFO_KIND(t->info))))
			continue;
		*out = ctx->name_map[i].id;
		return 0;
	}
	return -ENOENT;
}

u32 ti_type_size(const struct ti_ctx *ctx, u32 id)
{
	const struct btf_type *t;
	u32 kind;
	int idx;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0)
		return ti_reg_size(idx);

	t = ctx_type(ctx, id);
	if (!t)
		return 0;
	kind = BTF_INFO_KIND(t->info);

	switch (kind) {
	case BTF_KIND_PTR:
		return sizeof(void *);
	case BTF_KIND_ARRAY: {
		const struct btf_array *a = (const struct btf_array *)(t + 1);

		return ti_type_size(ctx, a->type) * a->nelems;
	}
	default:
		return t->size;
	}
}

int ti_follow(const struct ti_ctx *ctx, u32 id, u32 *out)
{
	u32 cur = id;
	int depth = 0;

	if (ti_reg_id_to_idx(id) >= 0) {
		*out = id;
		return 0;
	}

	while (cur) {
		const struct btf_type *t = ctx_type(ctx, cur);
		u32 kind;

		if (!t)
			return -ENOENT;
		kind = BTF_INFO_KIND(t->info);
		if (kind == BTF_KIND_TYPEDEF ||
		    (BTF_KIND_QUAL & BIT(kind))) {
			cur = t->type;
			depth++;
			if (depth > 64)
				return -ELOOP;
			continue;
		}
		break;
	}
	*out = cur;
	return 0;
}

int ti_member_count(const struct ti_ctx *ctx, u32 id)
{
	const struct btf_type *t;
	u32 kind;
	int ret;
	int idx;

	if (!ctx)
		return -EINVAL;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0)
		return ti_reg_mem_count(idx);

	ret = ti_follow(ctx, id, &id);
	if (ret)
		return ret;

	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
		return -EINVAL;
	return BTF_INFO_VLEN(t->info);
}

int ti_member_at(const struct ti_ctx *ctx, u32 id, u32 idx,
		 const char **name, u32 *type, u32 *bit_off, u32 *bit_sz)
{
	const struct btf_type *t;
	const struct btf_member *m;
	u32 kind;
	u32 vlen;
	int ret;
	int ridx;

	if (!ctx || !name || !bit_off || !bit_sz)
		return -EINVAL;

	ridx = ti_reg_id_to_idx(id);
	if (ridx >= 0) {
		int r = ti_reg_mem_at(ridx, idx, name, bit_off, bit_sz);

		if (!r && type)
			*type = 0;
		return r;
	}

	ret = ti_follow(ctx, id, &id);
	if (ret)
		return ret;

	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
		return -EINVAL;

	vlen = BTF_INFO_VLEN(t->info);
	if (idx >= vlen)
		return -ENOENT;
	m = (const struct btf_member *)(t + 1);
	if (BTF_INFO_KFLAG(t->info)) {
		*bit_off = BTF_MEMBER_BIT_OFF(m[idx].offset);
		*bit_sz = BTF_MEMBER_BIT_SZ(m[idx].offset);
	} else {
		*bit_off = m[idx].offset;
		*bit_sz = 0;
	}
	*name = ctx_str(ctx, m[idx].name_off);
	if (type)
		*type = m[idx].type;
	return 0;
}

int ti_member_off(const struct ti_ctx *ctx, u32 id, const char *member,
		  u32 *bit_off, u32 *bit_sz)
{
	const struct btf_type *t;
	const struct btf_member *m;
	u32 kind;
	u32 vlen;
	u32 i;
	int ret;
	int idx;

	if (!ctx || !member || !bit_off || !bit_sz)
		return -EINVAL;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0)
		return ti_reg_member(idx, member, bit_off, bit_sz);

	ret = ti_follow(ctx, id, &id);
	if (ret)
		return ret;

	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
		return -EINVAL;

	vlen = BTF_INFO_VLEN(t->info);
	m = (const struct btf_member *)(t + 1);
	for (i = 0; i < vlen; i++) {
		if (strcmp(ctx_str(ctx, m[i].name_off), member))
			continue;
		if (BTF_INFO_KFLAG(t->info)) {
			*bit_off = BTF_MEMBER_BIT_OFF(m[i].offset);
			*bit_sz = BTF_MEMBER_BIT_SZ(m[i].offset);
		} else {
			*bit_off = m[i].offset;
			*bit_sz = 0;
		}
		return 0;
	}
	return -ENOENT;
}

int ti_member_info(const struct ti_ctx *ctx, u32 id, const char *member,
		   u32 *type, u32 *bit_off, u32 *bit_sz)
{
	const struct btf_type *t;
	const struct btf_member *m;
	u32 kind;
	u32 vlen;
	u32 i;
	int ret;
	int idx;

	if (!ctx || !member || !type || !bit_off || !bit_sz)
		return -EINVAL;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0) {
		*type = 0;
		return ti_reg_member(idx, member, bit_off, bit_sz);
	}

	ret = ti_follow(ctx, id, &id);
	if (ret)
		return ret;

	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
		return -EINVAL;

	vlen = BTF_INFO_VLEN(t->info);
	m = (const struct btf_member *)(t + 1);
	for (i = 0; i < vlen; i++) {
		if (strcmp(ctx_str(ctx, m[i].name_off), member))
			continue;
		*type = m[i].type;
		if (BTF_INFO_KFLAG(t->info)) {
			*bit_off = BTF_MEMBER_BIT_OFF(m[i].offset);
			*bit_sz = BTF_MEMBER_BIT_SZ(m[i].offset);
		} else {
			*bit_off = m[i].offset;
			*bit_sz = 0;
		}
		return 0;
	}
	return -ENOENT;
}
