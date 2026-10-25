#!/usr/bin/env python3
"""Delete the vcpkg binary archives that an install did not use.

vcpkg's "files" binary cache only ever adds archives, and CI seeds a new cache key from the
previous one, so ports that left vcpkg.json (or older builds of ports that are still there)
would be carried forward forever. A package's ABI hash is the SHA-256 of its
share/<port>/vcpkg_abi_info.txt, and its archive is <abi[:2]>/<abi>.zip: every archive
whose name is not the ABI of an installed package is unused.

Usage: prune-vcpkg-bincache.py <binary cache dir> <vcpkg_installed dir>
"""

import hashlib
import sys
from pathlib import Path


def installed_abis(installed: Path) -> set[str]:
    return {
        hashlib.sha256(info.read_bytes()).hexdigest()
        for info in installed.glob("*/share/*/vcpkg_abi_info.txt")
    }


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    cache, installed = Path(sys.argv[1]), Path(sys.argv[2])
    keep = installed_abis(installed)
    if not keep:
        # Nothing installed means the install did not run here: pruning would empty the cache.
        print(f"No installed packages under {installed}: cache left as it is", file=sys.stderr)
        return 1

    archives = list(cache.glob("*/*.zip"))
    unused = [a for a in archives if a.stem not in keep]
    freed = 0
    for archive in unused:
        freed += archive.stat().st_size
        archive.unlink()
    for folder in cache.iterdir():
        if folder.is_dir() and not any(folder.iterdir()):
            folder.rmdir()

    missing = len(keep) - (len(archives) - len(unused))
    print(f"Kept {len(archives) - len(unused)} archives, deleted {len(unused)} "
          f"({freed / 2**20:.0f} MiB); {missing} installed packages have no archive")
    return 0


if __name__ == "__main__":
    sys.exit(main())
