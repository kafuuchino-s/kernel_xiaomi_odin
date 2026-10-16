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
submodules pinned to exact commits (see `../../.gitmodules`).

## How to build

```bash
git clone --recurse-submodules --branch mix4-5.4 https://github.com/kafuuchino-s/kernel_xiaomi_odin.git
cd kernel_xiaomi_odin
export PATH=/clang/bin:$PATH  # Android clang 11.0.2 (r383902b1)
./build.sh
```

`build.sh` reads this directory's `odin.json` and starts from
`baseline.config` unless `out/.config` already exists. It initializes the pinned
submodules, applies the SUSFS series from the susfs4ksu fork, and builds the
BakaSU fork with its 5.4 glue already committed. It also exports the frozen
build environment, sets the required config, applies the reproducibility
prefix maps and checks the resulting banner.

Use `out` inside the source tree to preserve byte reproducibility. See
[INTEGRATION.md](../../INTEGRATION.md) for the full dependency and patch-state
contract. After building, run the maintenance ABI audit against the matching
vendor symbols; `abi_gate` records the expected results.

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

## Source recipe and device configuration

This recipe records how to compile the ROM's kernel: toolchain, build environment,
ABI expectations and exact dependency pins. Device and userspace configuration
belongs in `mix4-maintenance`, separately from these public source forks. Build
artifacts, audit evidence and device validation records are maintained by the
release workflow outside these source repositories.
