#!/usr/bin/env python3
"""Compare stock vendor imports with a built kernel's exported symbol CRCs."""
import argparse
import json
from pathlib import Path
import sys


def symbols(text):
    result = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        crc, name, *_ = line.split()
        value = int(crc, 16)
        if not 0 <= value <= 0xFFFFFFFF:
            raise ValueError(f"Invalid CRC for {name}: {crc}")
        if name in result and result[name] != value:
            raise ValueError(f"Conflicting CRCs for {name}")
        result[name] = value
    return result


def audit(core, imports, vendor_exports, own_exports=()):
    mismatches, unresolved = [], []
    matched = supplied_by_vendor = 0
    for name, expected in imports.items():
        if name in core:
            if core[name] == expected:
                matched += 1
            else:
                mismatches.append({"symbol": name, "stock_crc": f"0x{expected:08x}",
                                   "built_crc": f"0x{core[name]:08x}"})
        elif name in vendor_exports:
            supplied_by_vendor += 1
        else:
            unresolved.append(name)
    return {"matched_kernel_imports": matched, "vendor_supplied_imports": supplied_by_vendor,
            "mismatches": mismatches, "unresolved": unresolved,
            "duplicate_exports": sorted(set(own_exports).intersection(core))}


def main():
    if sys.argv[1:] == ["--self-test"]:
        result = audit(symbols("0x1 module_layout vmlinux EXPORT_SYMBOL\n"),
                       symbols("0x2 module_layout\n0x3 vendor_func\n0x4 absent\n"),
                       {"vendor_func"})
        assert result == {"matched_kernel_imports": 0, "vendor_supplied_imports": 1,
                          "mismatches": [{"symbol": "module_layout", "stock_crc": "0x00000002",
                                          "built_crc": "0x00000001"}], "unresolved": ["absent"],
                          "duplicate_exports": []}
        assert audit({"module_layout": 1}, {"module_layout": 1}, set())["matched_kernel_imports"] == 1
        # Matching CRCs still cannot admit a module which re-exports a kernel symbol.
        assert audit({"display": 1}, {"display": 1}, {"display"}, {"display"})["duplicate_exports"] == ["display"]
        print("ABI audit self-test passed")
        return 0

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("symvers", type=Path)
    parser.add_argument("vendor_symbols", type=Path)
    args = parser.parse_args()
    core = symbols("\n".join(line for line in args.symvers.read_text().splitlines()
                             if len(line.split()) >= 3 and line.split()[2] == "vmlinux"))
    versions = sorted(args.vendor_symbols.glob("*.versions"))
    if not core or not versions:
        parser.error("Need nonempty vmlinux exports and stock vendor imports")
    exports = set()
    for path in args.vendor_symbols.glob("*.exports"):
        exports.update(path.read_text().splitlines())
    results = {path.name.removesuffix(".versions"): audit(
                   core, symbols(path.read_text()), exports,
                   path.with_suffix(".exports").read_text().splitlines())
               for path in versions}
    print(json.dumps({"kernel_module_layout_crc": f"0x{core['module_layout']:08x}",
                      "modules": results}, indent=2))
    return int(any(result["mismatches"] or result["unresolved"] or result["duplicate_exports"]
                   for result in results.values()))


if __name__ == "__main__":
    raise SystemExit(main())
