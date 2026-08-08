# type_info API

call ti_init once with a resolver that resolves kernel symbol names.
ti_init parses vmlinux BTF if present, finds the struct module offsets
(BTF or anchor), registers the module notifier for automatic module BTF
capture, and inits the registry. after ti_init use the query functions.

## Dependencies (KallRecon)

symbol resolution comes from KallRecon (see kallrecon/README.md and
kallrecon/doc/API.md). call find_kallsyms_base() first, then pass
kallrecon_klp (the kernel's own kallsyms_lookup_name) or
kallsyms_name_to_addr as ti_resolver.name_to_addr.

kallrecon/lib/core.o and kallrecon/lib/anchor.o are linked into the
module. only sprint_symbol is required from the kernel.

symbols resolved through the resolver:

- __start_BTF / __stop_BTF: vmlinux BTF blob bounds
- modules: module list head
- register_module_notifier / unregister_module_notifier: module
  notifier registration

any resolver implementation can be swapped in.

## Lifecycle

**int ti_init(struct ti_resolver *res)**

the one call that starts everything. 0 on success. -ENOTSUPP when
vmlinux BTF is unavailable; the anchor path is still used and the
library keeps working.

**void ti_exit(void)**

unregisters the notifier, frees captured module contexts and the
registry.

## Layout source

**struct ti_ctx *ti_base(void)**

the vmlinux context. query kernel structs against it.

**bool ti_btf_available(void)**

nonzero when vmlinux BTF was parsed. zero on no-BTF kernels.

**u32 ti_type_count(void)**

number of vmlinux BTF types. zero when BTF is unavailable.

**int ti_ctx_open(const void *blob, u32 size, struct ti_ctx **out)**

open any BTF blob (for example a module's distilled base) as its own
context. the caller owns the blob memory for the life of the context.

**void ti_ctx_close(struct ti_ctx *ctx)**

free a context created by ti_ctx_open. never pass ti_base() here.

## Query

**int ti_type_by_name(const struct ti_ctx *ctx, const char *name,
u32 kind_mask, u32 *out)**

find a type id by name. kind_mask filters by BTF kind
(BIT(BTF_KIND_STRUCT) for structs); 0 means no filter. hits the
registry when BTF misses and the name was registered. 0 on success,
-ENOENT when not found.

**u32 ti_type_size(const struct ti_ctx *ctx, u32 id)**

type size in bytes. pointers are 8, arrays are element * nelems.
0 when unknown.

**int ti_follow(const struct ti_ctx *ctx, u32 id, u32 *out)**

walk the typedef / const / volatile / restrict / type_tag chain and
return the underlying type id.

**int ti_member_off(const struct ti_ctx *ctx, u32 id,
const char *member, u32 *bit_off, u32 *bit_sz)**

find a member's bit offset. bit_sz is nonzero only for bitfields
(kflag layouts). byte offset = bit_off / 8.

**int ti_member_info(const struct ti_ctx *ctx, u32 id,
const char *member, u32 *type, u32 *bit_off, u32 *bit_sz)**

same as ti_member_off, also returns the member type id.

**int ti_member_count(const struct ti_ctx *ctx, u32 id)**

number of members. works on registry ids too.

**int ti_member_at(const struct ti_ctx *ctx, u32 id, u32 idx,
const char **name, u32 *type, u32 *bit_off, u32 *bit_sz)**

read the idx-th member: name, type, bit offset and bit size. -ENOENT
past the end. with ti_type_by_name + ti_member_count this exports a
full struct layout (member names and offsets).

synthetic ids >= 0x80000000 (TI_REG_ID_BASE) resolve to the registry;
all query functions handle them.

## Registry (collaborative registration)

**struct ti_member_desc { const char *name; u32 bit_off; u32 bit_sz; }**

one member description. use compile-time offsetof(x) * 8 for bit_off.

**int ti_reg_struct(const char *name, u32 size,
const struct ti_member_desc *members, u32 n)**

register a struct layout. exported, callable from any LKM. 0 on
success, -EINVAL on bad input, -ENOSPC when the table is full.
registered structs are found by ti_type_by_name and all query
functions with a synthetic id.

**void ti_unreg_struct(const char *name)**

remove a registered layout. call it from your module exit.

this is the only layout source for LKMs on no-BTF kernels. it does not
depend on the kernel BTF or on kernel version. verified working on
android12-5.10.

## Module

**struct ti_module { const char *name; unsigned int state;
unsigned long core_base; unsigned long core_size; void *mod; }**

one loaded module. state follows the kernel enum (LIVE 0, COMING 1,
GOING 2, UNFORMED 3). core_base/core_size are zero when the kernel has
no core_layout (6.6+ replaced it with struct module_memory mem[]).

**int ti_mod_lookup(const char *name, struct ti_ctx **out)**

find the captured BTF context of a loaded module. 0 when captured,
-ENOENT otherwise.

**int ti_mod_enum(int (*cb)(const struct ti_module *m, void *arg),
void *arg)**

walk the module list, call cb for each module. return cb's result to
stop early. works on all kernels, with or without BTF.

## Anchor scanning (CONFIG_TI_PUBLIC_ANCHOR)

built with TI_PUBLIC_ANCHOR=1, these primitives are exported for custom
anchor scans.

**int ti_safe_read(void *dst, const void *src, size_t sz)**

fault-safe memory read. zero on success, nonzero on failure.

**int ti_scan_bytes(const void *base, u32 range, const void *sample,
u32 len, u32 *off)**

find a byte pattern in a window.

**int ti_scan_u32(const void *base, u32 range, u32 val, u32 *off)**

find a u32 value in a window.

**int ti_scan_ptrpair(const void *base, u32 range, u32 *off)**

find two equal nonzero pointers next to each other.

**int ti_scan_ptrpair_rev(const void *base, u32 range, u32 *off)**

same, scanning backwards.

custom anchor pattern: scan for a known value (string, constant,
pointer pair), verify with cross-checks across several objects, cache
the offset. ti_bootstrap_task and ti_bootstrap_module are reference
implementations. the object you scan must stay mapped; the scan itself
never faults.

## No-BTF kernels (android12-5.10)

android12-5.10 GKI is built without CONFIG_DEBUG_INFO_BTF (android13
and later have it). this means:

- no vmlinux .BTF, so ti_init takes the anchor path and kernel struct
  layout comes from anchor scanning (task_struct five-piece, struct
  module)
- struct module has no btf_data field (compiled out), and modules are
  built without .BTF, so module BTF capture is unavailable
- the only layout source for later-loaded LKMs is collaborative
  registration (ti_reg_struct), which does not depend on BTF
- arbitrary third-party LKM layouts have no info source and cannot be
  recovered

## Load options (module params)

ti_cur_pid: current process pid, bootstrap input for the task anchor

ti_ref_pid: second process pid, used to cross-verify the pid anchor

ti_mod_anchor: force the struct module anchor path (testing)

ti_btf_mode: module BTF shape: 0 AUTO, 1 DISTILLED, 2 SPLIT,
3 STANDALONE. AUTO uses the module's btf_base_data when present,
otherwise vmlinux as base.

## Build options

TI_PUBLIC_ANCHOR=1: export the anchor scanning primitives (default off)
