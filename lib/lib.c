// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/compiler_types.h>

#include "btf.h"
#include "lib.h"
#include "type_info.h"
#include "anchor.h"

struct ti_mod_ent {
	char name[MODULE_NAME_LEN];
	struct ti_ctx *ctx;
	void *blob;
	void *mid_blob;
};

static struct ti_resolver ti_res;
static struct ti_ctx ti_base_ctx;
static bool ti_ready;

static struct mutex ti_mod_lock;
static struct notifier_block ti_mod_nb;
static struct ti_mod_ent *ti_mods;
static u32 ti_mod_cnt;
static u32 ti_mod_cap;

static u32 ti_m_data_off;
static u32 ti_m_data_sz_off;
static u32 ti_m_bdata_off;
static u32 ti_m_bdata_sz_off;
static bool ti_m_offs_ok;
static bool ti_m_has_bdata;
static unsigned long ti_nb_register;
static unsigned long ti_nb_unregister;

static __nocfi struct module *ti_call_find_module(unsigned long fn,
						  const char *name)
{
	return ((struct module *(*)(const char *))fn)(name);
}

static __nocfi int ti_call_register_nb(unsigned long fn,
				       struct notifier_block *nb)
{
	return ((int (*)(struct notifier_block *))fn)(nb);
}

static __nocfi int ti_call_unregister_nb(unsigned long fn,
					 struct notifier_block *nb)
{
	return ((int (*)(struct notifier_block *))fn)(nb);
}

struct ti_ctx *ti_base(void)
{
	return &ti_base_ctx;
}

u32 ti_type_count(void)
{
	if (!ti_ready)
		return 0;
	return ti_base_ctx.cur.type_cnt;
}

static void *ti_blob_dup(const void *src, u32 size)
{
	void *p;

	p = kvmalloc(size, GFP_KERNEL);
	if (!p)
		return NULL;
	memcpy(p, src, size);
	return p;
}

static void ti_mod_ent_free(struct ti_mod_ent *e)
{
	if (!e)
		return;
	if (e->ctx && e->ctx->base && e->ctx->base != &ti_base_ctx) {
		ti_ctx_close((struct ti_ctx *)e->ctx->base);
		e->ctx->base = NULL;
	}
	ti_ctx_close(e->ctx);
	vfree(e->mid_blob);
	vfree(e->blob);
	kfree(e);
}

static void ti_mod_ent_add(struct ti_mod_ent *e)
{
	struct ti_mod_ent *na;

	mutex_lock(&ti_mod_lock);
	if (ti_mod_cnt == ti_mod_cap) {
		u32 ncap = ti_mod_cap ? ti_mod_cap * 2 : 8;

		na = vmalloc(sizeof(*na) * ncap);
		if (!na)
			goto out_fail;
		if (ti_mods) {
			memcpy(na, ti_mods, sizeof(*na) * ti_mod_cnt);
			vfree(ti_mods);
		}
		ti_mods = na;
		ti_mod_cap = ncap;
	}
	ti_mods[ti_mod_cnt++] = *e;
	mutex_unlock(&ti_mod_lock);
	return;

out_fail:
	mutex_unlock(&ti_mod_lock);
	ti_mod_ent_free(e);
}

static void ti_mod_ent_del(const char *name)
{
	u32 i;

	mutex_lock(&ti_mod_lock);
	for (i = 0; i < ti_mod_cnt; i++) {
		if (strcmp(ti_mods[i].name, name))
			continue;
		ti_mod_ent_free(&ti_mods[i]);
		if (i + 1 < ti_mod_cnt)
			memmove(&ti_mods[i], &ti_mods[i + 1],
				sizeof(*ti_mods) * (ti_mod_cnt - i - 1));
		ti_mod_cnt--;
		break;
	}
	mutex_unlock(&ti_mod_lock);
}

static int ti_mod_capture(struct module *mod)
{
	unsigned long dptr;
	unsigned int dsize;
	struct ti_ctx *mid = NULL;
	struct ti_ctx *mc;
	struct ti_mod_ent ent;
	void *blob;
	void *mid_blob = NULL;
	int ret;

	dptr = *(unsigned long *)((char *)mod + ti_m_data_off);
	dsize = *(unsigned int *)((char *)mod + ti_m_data_sz_off);
	if (!dptr || !dsize)
		return 0;

	blob = ti_blob_dup((const void *)dptr, dsize);
	if (!blob)
		return 0;

	mc = kzalloc(sizeof(*mc), GFP_KERNEL);
	if (!mc) {
		vfree(blob);
		return 0;
	}
	strscpy(mc->name, mod->name, sizeof(mc->name));

	if (ti_m_has_bdata) {
		unsigned long bptr;
		unsigned int bsize;

		bptr = *(unsigned long *)((char *)mod + ti_m_bdata_off);
		bsize = *(unsigned int *)((char *)mod + ti_m_bdata_sz_off);
		if (bptr && bsize) {
			mid_blob = ti_blob_dup((const void *)bptr, bsize);
			if (mid_blob) {
				mid = kzalloc(sizeof(*mid), GFP_KERNEL);
				if (mid) {
					ret = ti_ctx_init(mid, mid_blob, bsize);
					if (ret) {
						kfree(mid);
						mid = NULL;
						vfree(mid_blob);
						mid_blob = NULL;
					} else {
						strscpy(mid->name, mod->name,
							sizeof(mid->name));
						mc->base = mid;
					}
				} else {
					vfree(mid_blob);
					mid_blob = NULL;
				}
			}
		}
	}
	if (!mc->base)
		mc->base = &ti_base_ctx;

	ret = ti_ctx_init(mc, blob, dsize);
	if (ret) {
		pr_warn("[type_info] module %s btf open failed: %d\n",
			mod->name, ret);
		ti_ctx_close(mid);
		vfree(mid_blob);
		kfree(mc);
		vfree(blob);
		return 0;
	}

	memset(&ent, 0, sizeof(ent));
	strscpy(ent.name, mod->name, sizeof(ent.name));
	ent.ctx = mc;
	ent.blob = blob;
	ent.mid_blob = mid_blob;
	ti_mod_ent_add(&ent);
	pr_info("[type_info] module %s btf captured: %u types\n", mod->name,
		mc->cur.type_cnt);
	return 0;
}

static int ti_mod_notify(struct notifier_block *nb, unsigned long ev,
			 void *data)
{
	struct module *mod = data;

	if (!ti_m_offs_ok)
		return NOTIFY_DONE;
	if (ev == MODULE_STATE_COMING)
		ti_mod_capture(mod);
	else if (ev == MODULE_STATE_GOING)
		ti_mod_ent_del(mod->name);
	return NOTIFY_DONE;
}

static int ti_mod_offs_init(void)
{
	u32 mod_id;
	u32 bit_off;
	u32 bit_sz;
	int ret;

	ret = ti_type_by_name(&ti_base_ctx, "module", BIT(BTF_KIND_STRUCT),
			      &mod_id);
	if (ret)
		return ret;

	ret = ti_member_off(&ti_base_ctx, mod_id, "btf_data", &bit_off,
			    &bit_sz);
	if (ret || bit_sz)
		return -ENOTSUPP;
	ti_m_data_off = bit_off / 8;

	ret = ti_member_off(&ti_base_ctx, mod_id, "btf_data_size", &bit_off,
			    &bit_sz);
	if (ret || bit_sz)
		return -ENOTSUPP;
	ti_m_data_sz_off = bit_off / 8;

	ti_m_has_bdata = !ti_member_off(&ti_base_ctx, mod_id, "btf_base_data",
					&bit_off, &bit_sz) && !bit_sz;
	if (ti_m_has_bdata) {
		ti_m_bdata_off = bit_off / 8;
		ret = ti_member_off(&ti_base_ctx, mod_id, "btf_base_data_size",
				    &bit_off, &bit_sz);
		if (ret || bit_sz) {
			ti_m_has_bdata = false;
		} else {
			ti_m_bdata_sz_off = bit_off / 8;
		}
	}
	ti_m_offs_ok = true;
	return 0;
}

int ti_init(struct ti_resolver *res)
{
	unsigned long start;
	unsigned long stop;
	u32 size;
	int ret;

	if (!res || !res->name_to_addr)
		return -EINVAL;
	ti_res = *res;
	ti_anchor_set_resolver(ti_res.name_to_addr);

	start = ti_res.name_to_addr("__start_BTF");
	stop = ti_res.name_to_addr("__stop_BTF");
	if (!start || !stop || stop <= start)
		return -ENOTSUPP;
	size = stop - start;

	ret = ti_ctx_init(&ti_base_ctx, (const void *)start, size);
	if (ret)
		return -ENOTSUPP;
	strscpy(ti_base_ctx.name, "vmlinux", sizeof(ti_base_ctx.name));
	ti_ready = true;

	mutex_init(&ti_mod_lock);
	ti_mod_offs_init();
	ti_nb_register = ti_res.name_to_addr("register_module_notifier");
	ti_nb_unregister = ti_res.name_to_addr("unregister_module_notifier");
	ti_mod_nb.notifier_call = ti_mod_notify;
	if (ti_nb_register && ti_nb_unregister) {
		if (ti_call_register_nb(ti_nb_register, &ti_mod_nb))
			pr_warn("[type_info] module notifier register failed\n");
	} else {
		pr_info("[type_info] module notifier unavailable, "
			"module btf disabled\n");
	}
	return 0;
}

void ti_exit(void)
{
	u32 i;

	if (!ti_ready)
		return;
	if (ti_nb_unregister)
		ti_call_unregister_nb(ti_nb_unregister, &ti_mod_nb);
	mutex_lock(&ti_mod_lock);
	for (i = 0; i < ti_mod_cnt; i++)
		ti_mod_ent_free(&ti_mods[i]);
	vfree(ti_mods);
	ti_mods = NULL;
	ti_mod_cnt = 0;
	ti_mod_cap = 0;
	mutex_unlock(&ti_mod_lock);
	ti_ctx_close(&ti_base_ctx);
	ti_ready = false;
}

int ti_mod_lookup(const char *name, struct ti_ctx **out)
{
	u32 i;
	int ret = -ENOENT;

	if (!name || !out)
		return -EINVAL;
	*out = NULL;
	mutex_lock(&ti_mod_lock);
	for (i = 0; i < ti_mod_cnt; i++) {
		if (strcmp(ti_mods[i].name, name))
			continue;
		*out = ti_mods[i].ctx;
		ret = 0;
		break;
	}
	mutex_unlock(&ti_mod_lock);
	return ret;
}
