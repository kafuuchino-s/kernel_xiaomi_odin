#!/usr/bin/env python3
"""Apply a pinned SUSFS series, or recognize its complete reverse state.

Usage: apply-susfs.py <kernel-root> <series>
Preflight uses a temporary index populated with the actual patch targets.
The real index is never changed. Check all patches in order, including patches
that depend on earlier entries; only then apply to the working tree.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def apply_series(root, series):
    root, series = Path(root).resolve(), Path(series).resolve()
    if not series.is_file():
        raise RuntimeError(f"SUSFS series missing: {series}; check the pinned susfs4ksu submodule")
    patches = []
    for number, line in enumerate(series.read_text().splitlines(), 1):
        fields = shlex.split(line, comments=True)
        if not fields:
            continue
        if len(fields) != 1:
            raise RuntimeError(f"{series}:{number}: expected one patch filename")
        patch = series.parent / fields[0]
        if not patch.is_file():
            raise RuntimeError(f"{series}:{number}: patch missing: {patch}")
        patches.append(str(patch))
    if not patches:
        raise RuntimeError(f"SUSFS series is empty: {series}")

    def git(*args, env=None, check=True):
        return subprocess.run(["git", "-C", str(root), *args], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=check)

    # --numstat -z reports both names for renames, and preserves unusual paths.
    targets = set()
    for patch in patches:
        records = iter(git("apply", "--numstat", "-z", patch).stdout.split(b"\0"))
        for record in records:
            if not record:
                continue
            name = record.split(b"	", 2)[2]
            if name:
                targets.add(os.fsdecode(name))
            else:
                targets.update((os.fsdecode(next(records)), os.fsdecode(next(records))))

    git_dir = os.fsdecode(git("rev-parse", "--absolute-git-dir").stdout).strip()
    with tempfile.TemporaryDirectory(prefix="susfs-check-", dir=git_dir) as directory:
        index = Path(directory) / "index"
        env = dict(os.environ, GIT_INDEX_FILE=str(index))
        git("read-tree", "HEAD", env=env)
        tracked = set(os.fsdecode(git("ls-files", "-z").stdout).split("\0"))
        present = sorted(name for name in targets
                         if name in tracked or os.path.lexists(root / name))
        if present:
            git("add", "--force", "--all", "--", *present, env=env)
        snapshot = Path(directory) / "snapshot"
        shutil.copyfile(index, snapshot)

        def preflight(reverse=False):
            shutil.copyfile(snapshot, index)
            for patch in reversed(patches) if reverse else patches:
                flags = ["--reverse"] if reverse else []
                result = git("apply", "--cached", "--whitespace=nowarn", *flags, patch,
                             env=env, check=False)
                if result.returncode:
                    return result.stderr.decode(errors="replace").strip()
            return None

        forward_error = preflight()
        if forward_error is None:
            git("apply", "--whitespace=nowarn", *patches)
            print(f"SUSFS: applied {len(patches)} patch(es) from {series}")
            return
        reverse_error = preflight(reverse=True)
        if reverse_error is None:
            print(f"SUSFS: complete series already applied ({len(patches)} patch(es)); skipping")
            return
        raise RuntimeError("SUSFS series is neither clean nor fully applied; "
                           "source files were not changed. Restore the affected files "
                           "to a complete clean or applied state before retrying.\n"
                           f"Forward check: {forward_error}\nReverse check: {reverse_error}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.splitlines()[2])
    try:
        apply_series(*sys.argv[1:])
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError) as error:
        detail = error.stderr.decode(errors="replace").strip() if isinstance(
            error, subprocess.CalledProcessError) else str(error)
        raise SystemExit(f"SUSFS: {detail}")
