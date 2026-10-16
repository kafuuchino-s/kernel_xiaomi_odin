# Build helpers

## `apply-susfs.py`

`build.sh` calls this helper with the kernel root and the pinned susfs4ksu
fork's `patches/5.4/2.3.0/series`. Each nonblank entry names one patch relative
to the series directory; `#` comments are supported. List patches in application
order.

The helper snapshots the actual patch targets into a temporary Git index,
checks the entire series in order, and applies it only after that succeeds.
It also checks the reversed series in reverse order to recognize an already
applied tree. Partial or conflicting states fail without changing source files.
The kernel's real Git index is preserved.

Run `python3 kernel-patches/test-apply-susfs.py` to exercise clean, repeated,
partial and conflicting application, dependent patches, and file creation
and deletion.

BakaSU's 5.4 glue is committed into the BakaSU fork. The former monolithic
`0001-bakasu-08966cd0-5.4-glue.patch` is replaced by that fork and is no longer
applied by this repository.

## `patch-mkcompile.py`

Stock `UTS_VERSION` ends in `f2fs-hash:f7f0f9542b0`. Our f2fs sources differ,
so `scripts/mkcompile_h` must honor `KBUILD_BUILD_F2FS_HASH` to reproduce the
stock banner. This idempotent helper installs that override if needed;
`build.sh` calls it before configuration. The same edit is committed directly
in `scripts/mkcompile_h`.

See [the integration guide](../INTEGRATION.md) for repository ownership,
pinning, and the normal build entry point.
