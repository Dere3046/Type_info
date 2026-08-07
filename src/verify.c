// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "btf.h"
#include "lib.h"
#include "type_info.h"
#include "anchor.h"
#include "../test/testmod.h"

static int vfails;

static void vcheck(int ok, const char *fmt, ...)
{
	va_list ap;

	if (ok)
		return;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
	vfails++;
}

#define CHECK(cond, fmt, ...) vcheck(cond, fmt, ##__VA_ARGS__)

#define T_INFO(kind, vlen, kflag) \
	((u32)(kind) << 24 | (vlen) | (kflag) << 31)

struct sbuf {
	void *p;
	u32 len;
	u32 str_base;
};

static u32 sb_u32(struct sbuf *s, u32 v)
{
	u32 off = s->len;

	memcpy((char *)s->p + off, &v, 4);
	s->len += 4;
	return off;
}

static u32 sb_raw(struct sbuf *s, const void *src, u32 n)
{
	u32 off = s->len;

	memcpy((char *)s->p + off, src, n);
	s->len += n;
	return off;
}

static u32 sb_str(struct sbuf *s, const char *str)
{
	return sb_raw(s, str, strlen(str) + 1) - s->str_base;
}

static void sb_type(struct sbuf *s, u32 name_off, u32 info, u32 size,
		    const void *extra, u32 extra_len)
{
	sb_u32(s, name_off);
	sb_u32(s, info);
	sb_u32(s, size);
	if (extra_len)
		sb_raw(s, extra, extra_len);
}

static void sb_hdr(struct sbuf *s, u32 type_off, u32 type_len, u32 str_len)
{
	u32 h[6] = { BTF_MAGIC | (1u << 16), 24, type_off, type_len, 0,
		     str_len };

	memcpy(s->p, h, sizeof(h));
}

static struct ti_ctx *synth_base(void **blobp, u32 *sizep, u32 *id_offp)
{
	struct sbuf s;
	struct ti_ctx *c;
	u32 off_int;
	u32 off_bits;
	u32 off_a;
	u32 off_b;
	u32 off_c;
	u32 off_myint;
	u32 off_arr4;
	u32 str_len;
	u32 type_off;
	u32 type_len;
	struct {
		u32 name_off;
		u32 type;
		u32 offset;
	} m[3];
	struct {
		u32 type;
		u32 index_type;
		u32 nelems;
	} arr;
	u32 int_data;

	s.p = vmalloc(8192);
	if (!s.p)
		return NULL;
	s.len = 24;
	s.str_base = s.len;

	off_int = sb_str(&s, "int");
	off_bits = sb_str(&s, "s_bits");
	off_a = sb_str(&s, "a");
	off_b = sb_str(&s, "b");
	off_c = sb_str(&s, "c");
	off_myint = sb_str(&s, "myint");
	off_arr4 = sb_str(&s, "arr4");
	str_len = s.len - s.str_base;

	type_off = s.len;
	int_data = BIT(31) | 32;
	sb_type(&s, off_int, T_INFO(BTF_KIND_INT, 0, 0), 4, &int_data, 4);

	m[0].name_off = off_a;
	m[0].type = 1;
	m[0].offset = (3u << 24) | 0;
	m[1].name_off = off_b;
	m[1].type = 1;
	m[1].offset = (5u << 24) | 3;
	m[2].name_off = off_c;
	m[2].type = 1;
	m[2].offset = 32;
	sb_type(&s, off_bits, T_INFO(BTF_KIND_STRUCT, 3, 1), 8, m, sizeof(m));

	sb_type(&s, off_myint, T_INFO(BTF_KIND_TYPEDEF, 0, 0), 1, NULL, 0);

	sb_type(&s, 0, T_INFO(BTF_KIND_PTR, 0, 0), 1, NULL, 0);

	arr.type = 1;
	arr.index_type = 1;
	arr.nelems = 4;
	sb_type(&s, off_arr4, T_INFO(BTF_KIND_ARRAY, 0, 0), 0, &arr,
		sizeof(arr));
	type_len = s.len - type_off;

	sb_hdr(&s, type_off - 24, type_len, str_len);

	*blobp = s.p;
	*sizep = s.len;
	*id_offp = 5;
	if (ti_ctx_open(s.p, s.len, &c)) {
		vfree(s.p);
		*blobp = NULL;
		return NULL;
	}
	return c;
}

static struct ti_ctx *synth_split(const struct ti_ctx *base, u32 id_off,
				  void **blobp, u32 *sizep)
{
	struct sbuf s;
	struct ti_ctx *c;
	u32 off_wrap;
	u32 off_inner;
	u32 str_len;
	u32 type_off;
	u32 type_len;
	struct {
		u32 name_off;
		u32 type;
		u32 offset;
	} m;

	s.p = vmalloc(8192);
	if (!s.p)
		return NULL;
	s.len = 24;
	s.str_base = s.len;

	off_wrap = sb_str(&s, "s_wrap");
	off_inner = sb_str(&s, "inner");
	str_len = s.len - s.str_base;

	type_off = s.len;
	m.name_off = base->cur.str_len + off_inner;
	m.type = 2;
	m.offset = 0;
	sb_type(&s, base->cur.str_len + off_wrap, T_INFO(BTF_KIND_STRUCT, 1, 0),
		4, &m, sizeof(m));
	type_len = s.len - type_off;

	sb_hdr(&s, type_off - 24, type_len, str_len);

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		vfree(s.p);
		return NULL;
	}
	c->base = base;
	if (ti_ctx_init(c, s.p, s.len)) {
		kfree(c);
		vfree(s.p);
		return NULL;
	}
	*blobp = s.p;
	*sizep = s.len;
	if (c->cur.id_off != id_off) {
		CHECK(0, "[type_info] synth: split id_off=%u want %u\n",
		      c->cur.id_off, id_off);
		ti_ctx_close(c);
		return NULL;
	}
	return c;
}

static void synth_check_base(const struct ti_ctx *c)
{
	u32 id;
	u32 bit_off;
	u32 bit_sz;

	CHECK(!ti_type_by_name(c, "s_bits", BIT(BTF_KIND_STRUCT), &id),
	      "[type_info] synth: s_bits not found\n");
	CHECK(!ti_member_off(c, id, "a", &bit_off, &bit_sz) &&
	      bit_off == 0 && bit_sz == 3,
	      "[type_info] synth: bitfield a off=%u sz=%u\n", bit_off, bit_sz);
	CHECK(!ti_member_off(c, id, "b", &bit_off, &bit_sz) &&
	      bit_off == 3 && bit_sz == 5,
	      "[type_info] synth: bitfield b off=%u sz=%u\n", bit_off, bit_sz);
	CHECK(!ti_member_off(c, id, "c", &bit_off, &bit_sz) &&
	      bit_off == 32 && bit_sz == 0,
	      "[type_info] synth: member c off=%u sz=%u\n", bit_off, bit_sz);
	CHECK(ti_type_size(c, id) == 8, "[type_info] synth: s_bits size=%u\n",
	      ti_type_size(c, id));

	CHECK(!ti_type_by_name(c, "myint", 0, &id),
	      "[type_info] synth: myint not found\n");
	CHECK(!ti_follow(c, id, &id) && id == 1,
	      "[type_info] synth: follow myint -> %u\n", id);

	CHECK(!ti_type_by_name(c, "arr4", 0, &id),
	      "[type_info] synth: arr4 not found\n");
	CHECK(ti_type_size(c, id) == 16, "[type_info] synth: arr4 size=%u\n",
	      ti_type_size(c, id));
}

static void synth_check_split(const struct ti_ctx *c)
{
	u32 id;
	u32 bit_off;
	u32 bit_sz;

	CHECK(!ti_type_by_name(c, "s_wrap", BIT(BTF_KIND_STRUCT), &id),
	      "[type_info] synth: s_wrap not found\n");
	CHECK(!ti_member_off(c, id, "inner", &bit_off, &bit_sz) &&
	      bit_off == 0 && bit_sz == 0,
	      "[type_info] synth: s_wrap.inner off=%u sz=%u\n", bit_off,
	      bit_sz);
	CHECK(!ti_follow(c, 2, &id) && id == 2,
	      "[type_info] synth: split follow base type -> %u\n", id);
	CHECK(!ti_member_off(c, 2, "a", &bit_off, &bit_sz) &&
	      bit_off == 0 && bit_sz == 3,
	      "[type_info] synth: split cross member a off=%u sz=%u\n",
	      bit_off, bit_sz);
}

static void verify_synth(void)
{
	struct ti_ctx *base;
	struct ti_ctx *split;
	void *bblob;
	void *sblob;
	u32 bsize;
	u32 ssize;
	u32 id_off;

	base = synth_base(&bblob, &bsize, &id_off);
	CHECK(base != NULL, "[type_info] synth: base build failed\n");
	if (!base)
		return;
	synth_check_base(base);

	split = synth_split(base, id_off, &sblob, &ssize);
	CHECK(split != NULL, "[type_info] synth: split build failed\n");
	if (split) {
		synth_check_split(split);
		ti_ctx_close(split);
		vfree(sblob);
	}
	ti_ctx_close(base);
	vfree(bblob);
}

static void verify_kernel(const struct ti_ctx *base)
{
	u32 id;
	u32 bit_off;
	u32 bit_sz;
	u32 off_pid;
	u32 off_comm;
	int ret;

	ret = ti_type_by_name(base, "task_struct", BIT(BTF_KIND_STRUCT), &id);
	CHECK(!ret, "[type_info] task_struct not found: %d\n", ret);
	if (ret)
		return;

	ret = ti_member_off(base, id, "pid", &bit_off, &bit_sz);
	CHECK(!ret, "[type_info] task_struct.pid lookup: %d\n", ret);
	if (!ret) {
		off_pid = bit_off / 8;
		CHECK(off_pid == offsetof(struct task_struct, pid) && !bit_sz,
		      "[type_info] pid btf=%u compile=%zu sz=%u\n", off_pid,
		      offsetof(struct task_struct, pid), bit_sz);
	}

	ret = ti_member_off(base, id, "comm", &bit_off, &bit_sz);
	CHECK(!ret, "[type_info] task_struct.comm lookup: %d\n", ret);
	if (!ret) {
		off_comm = bit_off / 8;
		CHECK(off_comm == offsetof(struct task_struct, comm) && !bit_sz,
		      "[type_info] comm btf=%u compile=%zu sz=%u\n", off_comm,
		      offsetof(struct task_struct, comm), bit_sz);
		if (off_comm <= 0x4000 && !bit_sz)
			CHECK(!strcmp(current->comm, (char *)current + off_comm),
			      "[type_info] comm mismatch: %s vs %s\n",
			      current->comm, (char *)current + off_comm);
	}

	CHECK(ti_type_size(base, id) == sizeof(struct task_struct),
	      "[type_info] task_struct size btf=%u compile=%zu\n",
	      ti_type_size(base, id), sizeof(struct task_struct));
}

static void verify_mod_cfg_struct(const struct ti_ctx *c)
{
	const u32 expect[7] = { 0, 32, 48, 56, 64, 128, 256 };
	const struct btf_type *t;
	const struct btf_member *m;
	u32 found_cfg = 0;
	u32 found_bits = 0;
	u32 i;
	u32 j;

	for (i = 1; i <= c->cur.type_cnt; i++) {
		u32 id = c->cur.id_off + i;
		u32 kind;

		t = btf_type(&c->cur, id);
		if (!t)
			break;
		kind = BTF_INFO_KIND(t->info);
		if (kind != BTF_KIND_STRUCT)
			continue;

		if (t->size == 40 && BTF_INFO_VLEN(t->info) == 7) {
			m = (const struct btf_member *)(t + 1);
			for (j = 0; j < 7; j++) {
				CHECK(BTF_MEMBER_BIT_OFF(m[j].offset) == expect[j],
				      "[type_info] mod: cfg member %u off=%u "
				      "want %u\n", j,
				      BTF_MEMBER_BIT_OFF(m[j].offset),
				      expect[j]);
			}
			found_cfg = 1;
		}
		if (t->size == 8 && BTF_INFO_VLEN(t->info) == 3 &&
		    BTF_INFO_KFLAG(t->info)) {
			m = (const struct btf_member *)(t + 1);
			CHECK(BTF_MEMBER_BIT_OFF(m[0].offset) == 0 &&
			      BTF_MEMBER_BIT_SZ(m[0].offset) == 3,
			      "[type_info] mod: bits.a off=%u sz=%u\n",
			      BTF_MEMBER_BIT_OFF(m[0].offset),
			      BTF_MEMBER_BIT_SZ(m[0].offset));
			CHECK(BTF_MEMBER_BIT_OFF(m[1].offset) == 3 &&
			      BTF_MEMBER_BIT_SZ(m[1].offset) == 5,
			      "[type_info] mod: bits.b off=%u sz=%u\n",
			      BTF_MEMBER_BIT_OFF(m[1].offset),
			      BTF_MEMBER_BIT_SZ(m[1].offset));
			CHECK(BTF_MEMBER_BIT_OFF(m[2].offset) == 32 &&
			      BTF_MEMBER_BIT_SZ(m[2].offset) == 0,
			      "[type_info] mod: bits.rest off=%u sz=%u\n",
			      BTF_MEMBER_BIT_OFF(m[2].offset),
			      BTF_MEMBER_BIT_SZ(m[2].offset));
			found_bits = 1;
		}
	}
	CHECK(found_cfg, "[type_info] mod: cfg struct not found\n");
	CHECK(found_bits, "[type_info] mod: bits struct not found\n");
}

static void verify_mod_cfg(const struct ti_ctx *c)
{
	u32 cfg_id;
	u32 bits_id;
	u32 bit_off;
	u32 bit_sz;
	u32 type;
	int ret;

	ret = ti_type_by_name(c, "ti_test_cfg", BIT(BTF_KIND_STRUCT), &cfg_id);
	if (ret) {
		pr_info("[type_info] mod: name lookup unavailable (%d), "
			"structural verify\n", ret);
		verify_mod_cfg_struct(c);
		return;
	}

	CHECK(ti_type_size(c, cfg_id) == sizeof(struct ti_test_cfg),
	      "[type_info] mod: cfg size btf=%u compile=%zu\n",
	      ti_type_size(c, cfg_id), sizeof(struct ti_test_cfg));

	ret = ti_member_info(c, cfg_id, "magic", &type, &bit_off, &bit_sz);
	CHECK(!ret && bit_off / 8 == offsetof(struct ti_test_cfg, magic) &&
	      !bit_sz,
	      "[type_info] mod: magic off=%u compile=%zu\n", bit_off / 8,
	      offsetof(struct ti_test_cfg, magic));

	ret = ti_member_info(c, cfg_id, "flags", &type, &bit_off, &bit_sz);
	CHECK(!ret && bit_off / 8 == offsetof(struct ti_test_cfg, flags) &&
	      !bit_sz,
	      "[type_info] mod: flags off=%u compile=%zu\n", bit_off / 8,
	      offsetof(struct ti_test_cfg, flags));

	ret = ti_member_info(c, cfg_id, "seq", &type, &bit_off, &bit_sz);
	CHECK(!ret && bit_off / 8 == offsetof(struct ti_test_cfg, seq) && !bit_sz,
	      "[type_info] mod: seq off=%u compile=%zu\n", bit_off / 8,
	      offsetof(struct ti_test_cfg, seq));

	ret = ti_member_info(c, cfg_id, "tag", &type, &bit_off, &bit_sz);
	CHECK(!ret && bit_off / 8 == offsetof(struct ti_test_cfg, tag) && !bit_sz,
	      "[type_info] mod: tag off=%u compile=%zu\n", bit_off / 8,
	      offsetof(struct ti_test_cfg, tag));

	ret = ti_member_info(c, cfg_id, "bits", &bits_id, &bit_off, &bit_sz);
	CHECK(!ret && bit_off / 8 == offsetof(struct ti_test_cfg, bits) &&
	      !bit_sz,
	      "[type_info] mod: bits off=%u compile=%zu\n", bit_off / 8,
	      offsetof(struct ti_test_cfg, bits));
	if (ret)
		return;

	ret = ti_member_off(c, bits_id, "a", &bit_off, &bit_sz);
	CHECK(!ret && bit_off == 0 && bit_sz == 3,
	      "[type_info] mod: bits.a off=%u sz=%u\n", bit_off, bit_sz);
	ret = ti_member_off(c, bits_id, "b", &bit_off, &bit_sz);
	CHECK(!ret && bit_off == 3 && bit_sz == 5,
	      "[type_info] mod: bits.b off=%u sz=%u\n", bit_off, bit_sz);
	ret = ti_member_off(c, bits_id, "rest", &bit_off, &bit_sz);
	CHECK(!ret && bit_off == 32 && bit_sz == 0,
	      "[type_info] mod: bits.rest off=%u sz=%u\n", bit_off, bit_sz);
}

int ti_verify_mods(void)
{
	struct ti_ctx *mc;
	int ret;

	vfails = 0;
	ret = ti_mod_lookup("testmod", &mc);
	if (ret) {
		pr_info("[type_info] mod: testmod not captured (%d), "
			"module may have no btf\n", ret);
		return 0;
	}

	verify_mod_cfg(mc);
	pr_info("[type_info] mod verify done, %d fail%s\n", vfails,
		vfails == 1 ? "" : "s");
	return vfails;
}
EXPORT_SYMBOL(ti_verify_mods);

extern int ti_cur_pid;
extern int ti_ref_pid;

static void verify_anchor(void)
{
	struct ti_task_offs offs;
	struct ti_boot_args args = {
		.comm = "init",
	};

	args.pid = ti_cur_pid;
	args.tgid = ti_cur_pid;
	args.ref_pid = ti_ref_pid;
	args.ref_tgid = ti_ref_pid;

	memset(&offs, 0, sizeof(offs));
	ti_bootstrap_task(&args, &offs);

	pr_info("[type_info] anchor: cur=%px pid=%u comm=%u rcred=%u "
		"cred=%u tasks=%u mm=%u\n",
		ti_current(), offs.off_pid, offs.off_comm,
		offs.off_real_cred, offs.off_cred, offs.off_tasks,
		offs.off_mm);
	pr_info("[type_info] anchor: compile pid=%zu comm=%zu rcred=%zu "
		"cred=%zu tasks=%zu mm=%zu\n",
		offsetof(struct task_struct, pid),
		offsetof(struct task_struct, comm),
		offsetof(struct task_struct, real_cred),
		offsetof(struct task_struct, cred),
		offsetof(struct task_struct, tasks),
		offsetof(struct task_struct, mm));

	CHECK(!offs.off_pid || offs.off_pid == offsetof(struct task_struct, pid),
	      "[type_info] anchor pid=%u compile=%zu\n", offs.off_pid,
	      offsetof(struct task_struct, pid));
	CHECK(!offs.off_comm || offs.off_comm == offsetof(struct task_struct, comm),
	      "[type_info] anchor comm=%u compile=%zu\n", offs.off_comm,
	      offsetof(struct task_struct, comm));
	CHECK(!offs.off_real_cred ||
	      offs.off_real_cred == offsetof(struct task_struct, real_cred),
	      "[type_info] anchor rcred=%u compile=%zu\n",
	      offs.off_real_cred, offsetof(struct task_struct, real_cred));
	CHECK(!offs.off_tasks ||
	      offs.off_tasks == offsetof(struct task_struct, tasks),
	      "[type_info] anchor tasks=%u compile=%zu\n", offs.off_tasks,
	      offsetof(struct task_struct, tasks));
	CHECK(!offs.off_mm || offs.off_mm == offsetof(struct task_struct, mm),
	      "[type_info] anchor mm=%u compile=%zu\n", offs.off_mm,
	      offsetof(struct task_struct, mm));
}

int verify_ti(void)
{
	struct ti_ctx *mc;

	vfails = 0;
	if (ti_base()->cur.type_cnt) {
		verify_synth();
		verify_kernel(ti_base());
	} else {
		pr_info("[type_info] btf unavailable, anchor path\n");
	}
	verify_anchor();

	mc = NULL;
	if (!ti_mod_lookup("testmod", &mc))
		verify_mod_cfg(mc);
	else
		pr_info("[type_info] testmod not captured yet\n");

	pr_info("[type_info] verify done, %d fail%s\n", vfails,
		vfails == 1 ? "" : "s");
	return vfails;
}
