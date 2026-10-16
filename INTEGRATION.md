# MIX 4 / Dream 3.0.308.0 build

Build the `mix4-5.4` branch on Linux with Android clang 11.0.2 (r383902b1),
Python 3, Git and the normal arm64 kernel build dependencies:

```bash
git clone --recurse-submodules --branch mix4-5.4 https://github.com/kafuuchino-s/kernel_xiaomi_odin.git
cd kernel_xiaomi_odin
export PATH=/clang/bin:$PATH
./build.sh
```

`/clang/bin` is the toolchain location in the maintenance container. Elsewhere,
put the same toolchain on `PATH`. The recipe is
[`configs/dream-3.0.308.0/odin.json`](configs/dream-3.0.308.0/odin.json), and
`build.sh` starts from the adjacent `baseline.config`. An existing `out/.config`
is reused with the required Root/SUSFS settings enforced.

Outputs are `out/arch/arm64/boot/Image` and `out/Module.symvers`. Keep the output
directory inside the source tree and name it `out` for byte reproducibility.
The script freezes the stock banner, checks the required config, preserves the
clang prefix maps, and builds Image plus modules. `techpack/Kbuild` records the
validated link order explicitly.

## Source ownership and pins

| Repository | Maintains |
| --- | --- |
| `kernel_xiaomi_odin` | Inkypen79 5.4.259 base, ROM/OEM adaptation, host hooks, configs and build entry point |
| `BakaSU` | Root framework with the existing 5.4 glue committed into the fork |
| `susfs4ksu` | Existing SUSFS 2.3.0 backport and the kernel patch series under `patches/5.4/2.3.0/` |

The kernel gitlinks pin exact BakaSU and susfs4ksu commits. The `mix4-5.4`
branch entries in `.gitmodules` describe their maintenance branches;
`build.sh` uses `git submodule update --init --recursive`, without `--remote`,
so a branch moving cannot silently change the build.

BakaSU is compiled through `drivers/kernelsu -> ../BakaSU/kernel`. Its pinned
fork already contains the 5.4 adaptation. The build applies no BakaSU patch.

SUSFS kernel changes are supplied by the pinned susfs4ksu commit. Before
configuration, `build.sh` reads `susfs4ksu/patches/5.4/2.3.0/series` and invokes
`kernel-patches/apply-susfs.py`:

- On a clean source tree, it preflights every patch in order, then applies the
  whole series to the working tree.
- When the complete series is already present, it skips application.
- A missing series, missing patch, partial application or conflicting edit
  stops the build before configuration. A failed preflight leaves source files
  unchanged and reports the forward and reverse failures.

Applied SUSFS changes remain visible in `git status`; they are derived build
inputs, maintained in the susfs4ksu fork. Before changing the SUSFS gitlink,
restore the files listed in that fork's patch documentation to their committed
kernel state and remove only the generated files named there. Then update the
gitlink and recipe together and run `build.sh` again. Preserve local edits
before restoring any files.

## Integration checks

Use `bash -n build.sh` for the shell entry point and
`python3 kernel-patches/test-apply-susfs.py` for patch application behavior.
The latter uses synthetic repositories and does not compile the kernel.

When updating either fork, commit the gitlink and matching recipe pin together.
Confirm the effective patched source still matches the validated release before
a full build. Accept the build after an ABI audit reports 114/114 QGKI modules,
`module_layout` CRC `0x894a26ae`, zero import mismatches, zero unresolved symbols
and zero duplicate exports. Run the repository's audit tool from the kernel
root after building:

```bash
python3 tools/mix4/audit_abi.py out/Module.symvers /path/to/vendor_symbols > /tmp/qgki-audit.json
```

`vendor_symbols` is an input directory generated or extracted from the target
ROM's vendor modules: one `.versions` import-CRC file and matching `.exports`
export-name file per module. These ROM-specific inputs are not part of this
source repository.
The tool also provides `python3 tools/mix4/audit_abi.py --self-test`.

## Upstream changes

This branch reorganizes the existing Dream 3.0.308.0 port. Its upstream anchors
remain kernel `05d251c8fc35d26e0f3143da56c3990c66f53303`, BakaSU
`08966cd0574d898a678a9bbe9a3ea0270e71ea6e` and SUSFS
`82f08351fadb8b19dd6591bd14634d02ad832145` (2.3.0). Fork commits package the
existing adaptations; changing these upstream versions requires a separate
port and validation.
