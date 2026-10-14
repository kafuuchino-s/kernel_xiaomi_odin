# kernel-patches/

Two things the build needs that upstream does not provide.

## `0001-bakasu-5b76b884-dream54-5.4.patch` — the 5.4 glue

BakaSU upstream targets newer kernels. At the commit this ROM pins
(`5b76b884c75f729a220bb317aa4a4fc78f0e0e9c`, version 35222 / UAPI 5) it does not
build on 5.4. This patch is the adaptation, and **the build does not work without
it** — `build.sh` applies it to the `BakaSU` submodule before compiling.

It touches 15 files:

```
kernel/Kbuild                      kernel/runtime/ksud_integration.c
kernel/Kconfig                     kernel/supercall/dispatch.c
kernel/compat/kernel_compat.c      kernel/supercall/supercall.c
kernel/core/init.c                 kernel/tools/kernel_compat.mk
kernel/feature/selinux_hide.c      kernel/tools/manual_hook_check.mk
kernel/feature/sucompat.c          kernel/hook/arm64/patch_memory.c
kernel/hook/setuid_hook.c          kernel/infra/symbol_resolver.c
kernel/manager/throne_tracker.h
```

Applying it yields BakaSU tree `849f0a2e7a031f17e0e18e3b71e827c20c4a6d87`.

### What in here is load-bearing

Not everything in a glue patch is glue, and the difference matters when the
submodule is bumped. These parts were checked with negative controls — the
upstream code was actually run and actually failed — so they are requirements:

| Change | Evidence if reverted |
|---|---|
| `sucompat.c`: `test_and_clear_thread_flag` instead of `test_thread_flag` | stale marker survives; negative control aborts with SIGABRT |
| `sucompat.c`: `clear_thread_flag(TIF_PROC_IN_KSU_EXECVE)` in `ksu_handle_execve` | as above |
| `supercall/dispatch.c`: `atomic_t` for the services event flag | plain `bool` under 6-way concurrency, 20000 rounds: two callers repeatedly both observe 1 |
| `kernel_compat.mk`: the `ifneq ($(filter y,$(CONFIG_KSU_MANUAL_HOOK) $(CONFIG_KSU_SUSFS)),)` guard | decides whether `KSU_COMPAT_NO_POST_EXECVE_HOOK` is set; wrong value changes which hook path compiles |
| `feature/selinux_hide.c`: dropping `__nocfi` | 5.4 assigns `ops->setprocattr` directly (`lsm_hooks.c:238`), so the call is typed; `__nocfi` was the bypass, not the fix. Keep it off. |
| `infra/symbol_resolver.c`, `hook/arm64/patch_memory.c`: 5.4 API adaptation | does not compile otherwise |

### What was removed, and why it is safe

Six changes were reverted because they had no negative control behind them:

- three `#include <linux/odin_ksu_hooks.h>` lines, in the three files that
  *define* those hooks (`sucompat.c`, `setuid_hook.c`, `supercall.c`). The header
  only restates their prototypes, so the include was a consistency check rather
  than a fix. The header itself stays — `runtime/ksud_integration.c` includes it
  and callers need the prototypes.
- a duplicated `"selinux/selinux.h"` include in `sucompat.c`, already included
  unconditionally a few lines below.
- a comment and an `$(info ...)` string in `kernel_compat.mk`, restored to
  upstream wording. The guard they sit next to is untouched.

Two further cosmetic changes were proposed and turned out to be unnecessary:
`fs/susfs.c` in this tree already has no trailing whitespace.

## `patch-mkcompile.py` — pin the banner's f2fs-hash

Stock `UTS_VERSION` ends in `f2fs-hash:f7f0f9542b0`, which is a hash of the
*stock* f2fs sources. This tree's f2fs sources differ, so `scripts/mkcompile_h`
computes `c38b0fe53dd` and `/proc/version` stops matching stock.

The script makes the hash overridable via `KBUILD_BUILD_F2FS_HASH` (falling back
to the computed value), which is what `build.sh` relies on. It is idempotent.

The same edit is committed directly into `scripts/mkcompile_h`; the script is
kept so the change can be re-applied if that file is ever reset from upstream.

## Not a patch

The working 5.4 SUSFS port is not here. It is committed in-tree as
`fs/susfs.c`, `include/linux/susfs.h` and `include/linux/susfs_def.h`, because
upstream `susfs4ksu` has no 5.4 branch to patch against.
