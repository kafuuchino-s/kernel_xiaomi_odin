# configs/

One directory per ROM release, each holding the complete recipe for building
that ROM's kernel.

```
configs/
└── dream-3.0.308.0/
    ├── odin.json         machine-readable recipe (the whole thing)
    ├── baseline.config   the .config this recipe is built from
    └── README.md         human notes for this ROM
```

## Why the kernel source is in the repo but the recipe is separate

This repo carries the kernel source itself, because a large part of it is a
device-specific OEM reconstruction that exists nowhere else. The recipe is kept
apart from the source so that adding a ROM release does not mean touching the
kernel tree: the tree is what you build, `configs/` is how you build it.

## Adding a new ROM release

1. Copy `configs/dream-3.0.308.0/` to `configs/dream-<new-version>/`.
2. Update `odin.json`. At minimum:
   - `rom`, `rom_full_name`, `android_version`
   - `rom_source.package` and every sha256 in `rom_source`
   - `banner.*` — re-read these off the new stock `boot.img`, do not carry them
     over by hand. A wrong banner is visible to anything that calls `uname -r`.
3. Refresh `baseline.config` from the new stock kernel.
4. **Re-audit the ABI before building.** The 114 QGKI modules come from the ROM's
   `vendor` partition, so a new ROM means a new ABI surface:
   ```
   python3 kernel/build/audit_abi.py <out>/Module.symvers <vendor-qgki-symbols>
   ```
   Expect 114/114 modules, `module_layout` 0x894a26ae, and zero mismatches,
   unresolved symbols and duplicate exports. If `module_layout` moved, the new
   ROM needs a fresh adaptation rather than a rebuild.
5. Build with `build.sh`, which reads the recipe from the path given to it.

## Not in here

The recipe is only about reproducing a kernel image whose observable surface
matches the stock ROM, which is a prerequisite for anything else.
