// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_LIB_H
#define TYPEMOD_LIB_H

#include <linux/types.h>

#include "btf.h"

struct ti_nsent {
	u32 name_off;
	u32 id;
};

struct ti_ctx {
	char name[64];
	struct btf_cursor cur;
	const struct ti_ctx *base;
	struct ti_nsent *name_map;
	u32 name_cnt;
};

int ti_ctx_init(struct ti_ctx *c, const void *blob, u32 size);

void ti_verify_captured(const struct ti_ctx *mc);
void ti_verify_enum(void);
void ti_verify_reg(void);

#endif
