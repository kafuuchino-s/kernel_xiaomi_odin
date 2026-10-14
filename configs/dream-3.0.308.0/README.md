# `configs/` — build recipes

One directory per ROM release: `configs/<rom>/<device>.json`.

Each file is a **complete, reproducible recipe** for building this kernel against
one ROM + device pair: every frozen value (banner, build env, toolchain, ABI gate)
plus the pinned Root/SUSFS commits.

```
configs/
└── dream-3.0.308.0/
    ├── odin.json      ← machine-readable recipe
    └── README.md      ← this file
```

## Why this exists

The upstream Inkypen79 tree has no OEM subsystems; this fork carries ~5.5k lines
of Dream OEM reconstruction (MIGT/GLK, PKG runtime, PERF/SF binder, CLD/UFS,
odin power). That source **must** live in-tree — a manifest-only CI approach
(like `OnePlus_BakaSU_SUSFS`) cannot reproduce it. What we borrow from that
convention is only the `configs/` idea: keep the *recipe* next to the source.

The kernel source itself is committed here. `BakaSU/` and `susfs4ksu/` are
submodules pinned to exact commits (see `../.gitmodules`).

## How to build

```bash
git clone --recursive https://github.com/kafuuchino-s/kernel_xiaomi_odin
cd kernel_xiaomi_odin
git checkout <tag>              # e.g. dream-3.0.308.0-r1
```

Then follow the values in `configs/<rom>/<device>.json`:

1. `compiler.docker_volume` holds the exact clang (`compiler.id`). Mount it at
   `/clang` and put `/clang/bin` first on `PATH`.
2. Export every `build_env.*` variable verbatim. These freeze the banner; if any
   changes, `banner` will no longer match stock and the build is invalid.
3. `scripts/config --file <out>/.config --set-str LOCALVERSION "<localversion>"`
   and `--disable LOCALVERSION_AUTO` (stock has `LOCALVERSION_AUTO=y`, which would
   otherwise append `-g<sha>-dirty` and break the banner).
4. `make -C . O=<out> <make_args...> olddefconfig`
5. `make -C . O=<out> <make_args...> KCFLAGS='<kbuild_kcflags>' -j8 <targets...>`
6. Gate with `abi_gate.audit_tool` against `abi_gate.audit_input`. All four
   requirements in `abi_gate` must hold.

Verify the result against `banner`:

```bash
grep UTS_RELEASE <out>/include/generated/utsrelease.h
# 5.4.259-qgki-g8cc268997371
```

## Adding a new ROM release

```bash
mkdir configs/<new-rom>
cp configs/dream-3.0.308.0/odin.json configs/<new-rom>/odin.json
```

Then re-derive, do **not** copy blindly:

| Field | How to re-derive |
|---|---|
| `rom`, `rom_full_name`, `android_version` | the new ROM |
| `banner`, `localversion`, `uts_version` | `strings` the new stock `boot.img`, or boot it and read `/proc/version` |
| `build_env.*` | the same source as `banner` — they are what produce it |
| `rom_source.images_sha256` | `sha256sum` the new `boot.img` / `vendor_boot.img` / `dtbo.img` / `super.img` |
| `rom_source.stock_config_sha256` | extract IKCONFIG from the new `boot.img`; confirm it matches the device's `/proc/config.gz` |
| `base_commit` | the upstream branch/commit the new ROM's kernel is based on |
| `abi_gate.module_layout_crc` | re-run `audit_abi.py` against the new ROM's `vendor-qgki-symbols/` |

**Every ROM needs its own `boot`/`vendor` interface check.** Vendor modules are
bound to the ROM; a kernel built for one ROM is not interchangeable with another.
Never reuse a previous release's `abi_gate` without re-auditing.

Keep `_comment` and the `*_note` fields: they record *why* a value is frozen.

## Relationship to `mix4-maintenance/`

Different jobs, deliberately overlapping fields:

| | this repo (`configs/`) | `mix4-maintenance/releases/<tag>/MANIFEST.json` |
|---|---|---|
| answers | *how do I compile this ROM's kernel?* | *what did we ship, and what was verified on device?* |
| side | source | delivery |
| contains | toolchain, build env, ABI gate, upstream pins | artifact hashes, device verification, rollback, limitations |

Do not try to de-duplicate the two. Update both when a release is cut.
