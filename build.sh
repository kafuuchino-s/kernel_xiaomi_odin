#!/usr/bin/env bash
# Reproducible build for the Dream OS3.0.308.0 / MIX 4 (odin) kernel.
#
#   ./build.sh [output-dir]        # default: out
#
# Reads the recipe from configs/dream-3.0.308.0/odin.json. Requires the
# toolchain described there (Android clang 11.0.2) on PATH, and a kernel
# config to start from.
#
# The BakaSU submodule is pinned to an upstream commit that does NOT build on
# 5.4 by itself: patches/0001-* is the 5.4 glue this device needs. Applying it
# is not optional. See patches/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$ROOT/out}"
RECIPE="$ROOT/configs/dream-3.0.308.0/odin.json"
BASE_CONFIG="${BASE_CONFIG:-$ROOT/configs/dream-3.0.308.0/baseline.config}"

command -v python3 >/dev/null || { echo "python3 required" >&2; exit 1; }

# ---- 1. submodules -------------------------------------------------------
# BakaSU/kernel is what drivers/kernelsu points at. Without it the tree is a
# dangling symlink and nothing compiles.
git -C "$ROOT" submodule update --init --recursive

# ---- 2. apply the 5.4 glue to the BakaSU submodule ----------------------
# Idempotent: skip if the submodule already carries the adaptation.
if ! git -C "$ROOT/BakaSU" apply --reverse --check \
        "$ROOT/kernel-patches/0001-bakasu-5b76b884-dream54-5.4.patch" 2>/dev/null; then
    git -C "$ROOT/BakaSU" apply "$ROOT/kernel-patches/0001-bakasu-5b76b884-dream54-5.4.patch"
fi

# ---- 3. pin the banner's f2fs-hash --------------------------------------
# Stock UTS_VERSION ends in f2fs-hash:f7f0f9542b0, which is a hash of the
# stock f2fs sources. Our tree's f2fs sources differ, so the hash must be
# pinned or /proc/version stops matching stock byte-for-byte.
python3 "$ROOT/kernel-patches/patch-mkcompile.py" "$ROOT/scripts/mkcompile_h"

# ---- 4. read the recipe --------------------------------------------------
read_recipe() { python3 -c "import json,sys;d=json.load(open(sys.argv[1]));
for k in sys.argv[2].split('.'):d=d[k]
print(d if not isinstance(d,(list,dict)) else json.dumps(d))" "$RECIPE" "$1"; }

export KBUILD_BUILD_USER="$(read_recipe build_env.KBUILD_BUILD_USER)"
export KBUILD_BUILD_HOST="$(read_recipe build_env.KBUILD_BUILD_HOST)"
export KBUILD_BUILD_TIMESTAMP="$(read_recipe build_env.KBUILD_BUILD_TIMESTAMP)"
export KBUILD_BUILD_VERSION="$(read_recipe build_env.KBUILD_BUILD_VERSION)"
export KBUILD_BUILD_F2FS_HASH="$(read_recipe build_env.KBUILD_BUILD_F2FS_HASH)"
export LOCALVERSION=""
export GIT_NO_LAZY_FETCH=1 GIT_TERMINAL_PROMPT=0
unset KCFLAGS KCPPFLAGS CFLAGS_KERNEL CFLAGS_MODULE

# ---- 5. configure --------------------------------------------------------
mkdir -p "$OUT"
[ -e "$OUT/.config" ] || cp "$BASE_CONFIG" "$OUT/.config"
"$ROOT/scripts/config" --file "$OUT/.config" --set-str LOCALVERSION "$(read_recipe banner.localversion)"
"$ROOT/scripts/config" --file "$OUT/.config" --disable LOCALVERSION_AUTO
"$ROOT/scripts/config" --file "$OUT/.config" \
    --enable KSU --enable KSU_SUSFS \
    --disable KSU_MANUAL_HOOK --disable KSU_TRACEPOINT_HOOK --disable KPM \
    --disable KSU_MANUAL_HOOK_AUTO_SETUID_HOOK \
    --disable KSU_MANUAL_HOOK_AUTO_INITRC_HOOK \
    --disable KSU_MANUAL_HOOK_AUTO_INPUT_HOOK
for feature in SUS_PATH SUS_MOUNT SUS_KSTAT SPOOF_UNAME ENABLE_LOG \
               HIDE_KSU_SUSFS_SYMBOLS SPOOF_CMDLINE_OR_BOOTCONFIG \
               OPEN_REDIRECT SUS_MAP; do
    "$ROOT/scripts/config" --file "$OUT/.config" --enable "KSU_SUSFS_$feature"
done

mapfile -t ARGS < <(python3 -c "import json,sys;print('\n'.join(json.load(open(sys.argv[1]))['make_args']))" "$RECIPE")

make -C "$ROOT" O="$OUT" "${ARGS[@]}" olddefconfig

# ---- 6. gate the config before spending an hour compiling ---------------
for o in CFI_CLANG SHADOW_CALL_STACK MODVERSIONS SECURITY_SELINUX KSU KSU_SUSFS; do
    grep -Fxq "CONFIG_${o}=y" "$OUT/.config" || { echo "MISSING CONFIG_${o}=y" >&2; exit 3; }
done
grep -q "^CONFIG_STACKPROTECTOR_PER_TASK=y" "$OUT/.config" && { echo "STACKPROTECTOR_PER_TASK leaked" >&2; exit 4; }
grep -Fxq "CONFIG_LOCALVERSION=\"$(read_recipe banner.localversion)\"" "$OUT/.config" || { echo "LOCALVERSION wrong" >&2; exit 5; }

# ---- 7. build ------------------------------------------------------------
make -C "$ROOT" O="$OUT" "${ARGS[@]}" \
     KCFLAGS="$(read_recipe kbuild_kcflags)" -j"$(nproc)" \
     $(python3 -c "import json,sys;print(' '.join(json.load(open(sys.argv[1]))['targets']))" "$RECIPE")

# ---- 8. verify the banner ------------------------------------------------
EXPECTED="$(read_recipe banner.uts_release)"
ACTUAL="$(sed -n 's/^#define UTS_RELEASE "\(.*\)"$/\1/p' "$OUT/include/generated/utsrelease.h")"
echo "UTS_RELEASE: $ACTUAL"
[ "$ACTUAL" = "$EXPECTED" ] || { echo "BANNER MISMATCH: expected $EXPECTED" >&2; exit 6; }
grep UTS_VERSION "$OUT/include/generated/compile.h"
sha256sum "$OUT/arch/arm64/boot/Image" "$OUT/Module.symvers"
echo "BUILD_OK"
