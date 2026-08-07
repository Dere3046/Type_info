// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_TYPE_INFO_H
#define TYPEMOD_TYPE_INFO_H

#include <linux/types.h>

struct ti_resolver {
	unsigned long (*name_to_addr)(const char *name);
};

struct ti_ctx;

int ti_init(struct ti_resolver *res);
void ti_exit(void);
struct ti_ctx *ti_base(void);
u32 ti_type_count(void);

int ti_ctx_open(const void *blob, u32 size, struct ti_ctx **out);
void ti_ctx_close(struct ti_ctx *ctx);

int ti_type_by_name(const struct ti_ctx *ctx, const char *name,
		    u32 kind_mask, u32 *out);
u32 ti_type_size(const struct ti_ctx *ctx, u32 id);
int ti_follow(const struct ti_ctx *ctx, u32 id, u32 *out);
int ti_member_off(const struct ti_ctx *ctx, u32 id, const char *member,
		  u32 *bit_off, u32 *bit_sz);
int ti_member_info(const struct ti_ctx *ctx, u32 id, const char *member,
		   u32 *type, u32 *bit_off, u32 *bit_sz);

int ti_mod_lookup(const char *name, struct ti_ctx **out);
int ti_verify_mods(void);

#endif
