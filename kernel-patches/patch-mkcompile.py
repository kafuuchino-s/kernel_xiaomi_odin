#!/usr/bin/env python3
"""Pin f2fs-hash in scripts/mkcompile_h so the banner matches stock byte-for-byte.

Stock UTS_VERSION ends in `f2fs-hash:f7f0f9542b0`, a hash of the *stock* f2fs
sources. Our tree's f2fs sources differ, so mkcompile_h computes a different
hash (c38b0fe53dd) and /proc/version stops matching stock. This lets the hash be
supplied via KBUILD_BUILD_F2FS_HASH, falling back to the computed value.

Usage: patch-mkcompile.py [path/to/scripts/mkcompile_h]

Idempotent: running twice is a no-op.
"""
import sys
from pathlib import Path

p = Path(sys.argv[1] if len(sys.argv) > 1 else "scripts/mkcompile_h")
lines = p.read_text().split("\n")

if any("KBUILD_BUILD_F2FS_HASH" in line for line in lines):
    print(f"{p}: already patched")
    raise SystemExit(0)

out, i, patched = [], 0, False
while i < len(lines):
    if lines[i].startswith("F2FS_HASH=$(sha1sum source/fs/f2fs/"):
        # The original is two physical lines joined by a trailing backslash.
        assert lines[i].endswith("\\"), repr(lines[i])
        assert lines[i + 1].lstrip().startswith("| awk"), repr(lines[i + 1])
        out.append('if [ -n "$KBUILD_BUILD_F2FS_HASH" ]; then')
        out.append('\tF2FS_HASH="$KBUILD_BUILD_F2FS_HASH"')
        out.append('else')
        out.append('\tF2FS_HASH=$(sha1sum source/fs/f2fs/[[:lower:]]* source/include/linux/f2fs_fs.h\')
        out.append('\t\t| awk \'{print $1}\' | sha1sum | awk \'{print $1}\')')
        out.append('fi')
        i += 2
        patched = True
        continue
    out.append(lines[i])
    i += 1

if not patched:
    raise SystemExit(f"{p}: F2FS_HASH block not found - upstream changed?")
p.write_text("\n".join(out))
print(f"{p}: patched")
