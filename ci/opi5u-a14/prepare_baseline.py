#!/usr/bin/env python3
"""Extract the unchanged A14 metadata/boot baseline for cloud boot-only OTAs.

Run once on the proven A14 host. This does not rebuild Android or sign OTAs.
Afterwards verify the result with the existing AOSP ota_from_target_files tool.
"""

import argparse
import hashlib
import sys
import zipfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target_files", type=Path, help="BOARD-qualified A14 target-files ZIP")
    parser.add_argument("output", type=Path, help="New, reduced target-files ZIP")
    parser.add_argument("--boot-sha256", required=True, help="Known-good boot.img SHA256")
    args = parser.parse_args()

    if args.output.exists():
        parser.error(f"Output already exists: {args.output}")
    if not args.target_files.is_file():
        parser.error(f"Missing target-files: {args.target_files}")
    args.output.parent.mkdir(parents=True, exist_ok=True)

    required = {
        "IMAGES/boot.img", "META/misc_info.txt", "META/ab_partitions.txt",
        "META/otakeys.txt", "META/update_engine_config.txt",
        "SYSTEM/build.prop", "VENDOR/build.prop",
    }
    selected = []
    try:
        with zipfile.ZipFile(args.target_files) as src:
            names = set(src.namelist())
            missing = sorted(required - names)
            if missing:
                raise ValueError(f"Required target-files entries missing: {missing}")
            if "boot" not in src.read("META/ab_partitions.txt").decode().split():
                raise ValueError("Target-files do not declare boot as an A/B partition")
            with src.open("IMAGES/boot.img") as boot:
                digest = hashlib.file_digest(boot, "sha256").hexdigest()
            if digest.lower() != args.boot_sha256.lower():
                raise ValueError(f"Boot mismatch: {digest} != {args.boot_sha256}")

            # Retain all OTA metadata plus per-partition build properties.
            # Non-boot partition images make the input multi-gigabyte and are
            # never included in a --partial boot payload.
            selected = [n for n in src.namelist() if n.startswith(("META/", "OTA/"))
                        or n == "IMAGES/boot.img" or n.endswith("/build.prop")]
            with zipfile.ZipFile(args.output, "w", allowZip64=True) as dst:
                for name in selected:
                    info = src.getinfo(name)
                    with src.open(info) as inp, dst.open(info, "w", force_zip64=True) as out:
                        while True:
                            chunk = inp.read(1024 * 1024)
                            if not chunk:
                                break
                            out.write(chunk)
    except Exception:
        args.output.unlink(missing_ok=True)
        raise

    print(f"Source: {args.target_files}")
    print(f"Baseline: {args.output} ({args.output.stat().st_size:,} bytes)")
    print(f"Retained entries: {len(selected)}")
    print(f"Boot SHA-256: {digest}")
    print("Baseline prepared; NOT OTA-qualified until the existing releasetools accepts it.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, zipfile.BadZipFile) as exc:
        sys.exit(f"ERROR: {exc}")
