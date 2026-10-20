#!/usr/bin/env python3
"""Repack the BOARD-passed Android 14 boot header v2 with a new kernel Image.

Refuse to continue unless the original boot.img is reproduced byte-for-byte
from its existing kernel, ramdisk, second image and header parameters.
"""
import argparse
import hashlib
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

BOOT_SHA = "6e235eb05bc1492e58e811c0e30831e5d1ca93dcd24df6c94aebe381bc2924fd"
BOOT_MAX = 67108864


def sha(data):
    return hashlib.sha256(data).hexdigest()


def aligned(n, size):
    return (n + size - 1) // size * size


def unpack_boot(boot):
    if len(boot) < 1660 or boot[:8] != b"ANDROID!":
        raise ValueError("not an Android boot image")
    kernel_size, ramdisk_size, second_size = (struct.unpack_from("<I", boot, o)[0] for o in (8, 16, 24))
    page_size, version, osver = struct.unpack_from("<III", boot, 36)
    if version != 2 or page_size not in (2048, 4096, 8192, 16384):
        raise ValueError(f"unsupported boot header v{version}, page {page_size}")
    recovery_dtbo_size = struct.unpack_from("<I", boot, 1632)[0]
    dtb_size = struct.unpack_from("<I", boot, 1648)[0]
    if recovery_dtbo_size or dtb_size:
        raise ValueError("unexpected recovery DTBO or DTB; repacking must preserve these explicitly")
    year, month = 2000 + ((osver >> 4) & 0x7f), osver & 15
    major, minor, patch = (osver >> 25) & 0x7f, (osver >> 18) & 0x7f, (osver >> 11) & 0x7f
    if not 1 <= month <= 12:
        raise ValueError("invalid boot patch month")
    cmdline = (boot[64:576].split(b"\0", 1)[0] + boot[608:1632].split(b"\0", 1)[0]).decode("utf-8")
    board = boot[48:64].split(b"\0", 1)[0].decode("ascii")
    kernel_at = page_size
    ramdisk_at = kernel_at + aligned(kernel_size, page_size)
    second_at = ramdisk_at + aligned(ramdisk_size, page_size)
    if second_at + second_size > len(boot):
        raise ValueError("truncated boot partition image")
    return {
        "kernel": boot[kernel_at:kernel_at + kernel_size],
        "ramdisk": boot[ramdisk_at:ramdisk_at + ramdisk_size],
        "second": boot[second_at:second_at + second_size],
        "page": page_size, "cmdline": cmdline, "board": board,
        "version": f"{major}.{minor}.{patch}", "patch": f"{year:04d}-{month:02d}-01",
    }


def make_boot(tool, fields, kernel, tmp, name):
    k = tmp / f"{name}.kernel"
    ramdisk = tmp / "ramdisk"
    second = tmp / "second"
    output = tmp / f"{name}.img"
    k.write_bytes(kernel)
    if not ramdisk.exists():
        ramdisk.write_bytes(fields["ramdisk"])
        second.write_bytes(fields["second"])
    cmd = [str(tool), "--kernel", str(k), "--ramdisk", str(ramdisk),
           "--second", str(second), "--cmdline", fields["cmdline"],
           "--header_version", "2", "--pagesize", str(fields["page"]),
           "--os_version", fields["version"],
           "--os_patch_level", fields["patch"], "--output", str(output)]
    if fields["board"]:
        cmd += ["--board", fields["board"]]
    subprocess.run(cmd, check=True)
    return output.read_bytes()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--kernel", type=Path, required=True)
    p.add_argument("--mkbootimg", type=Path, required=True)
    p.add_argument("--output-boot", type=Path, required=True)
    p.add_argument("--output-target-files", type=Path, required=True)
    args = p.parse_args()
    for f in (args.baseline, args.kernel, args.mkbootimg):
        if not f.is_file():
            p.error(f"missing input: {f}")
    for f in (args.output_boot, args.output_target_files):
        if f.exists():
            p.error(f"refusing to overwrite: {f}")
    with zipfile.ZipFile(args.baseline) as src:
        reference = src.read("IMAGES/boot.img")
    if sha(reference) != BOOT_SHA:
        raise ValueError("baseline boot is not the BOARD-qualified image")
    fields = unpack_boot(reference)
    args.output_boot.parent.mkdir(parents=True, exist_ok=True)
    args.output_target_files.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="opi5u-repack-") as temp:
        tmp = Path(temp)
        reproduced = make_boot(args.mkbootimg, fields, fields["kernel"], tmp, "reference")
        if reproduced != reference:
            raise ValueError(f"repacking reference boot changed bytes: {sha(reproduced)} != {BOOT_SHA}")
        updated = make_boot(args.mkbootimg, fields, args.kernel.read_bytes(), tmp, "new")
    if len(updated) > BOOT_MAX:
        raise ValueError(f"new boot image exceeds {BOOT_MAX} bytes")
    # Preserve existing OTA metadata. Only IMAGES/boot.img changes.
    with zipfile.ZipFile(args.baseline) as src, zipfile.ZipFile(args.output_target_files, "w", allowZip64=True) as dst:
        for item in src.infolist():
            dst.writestr(item, updated if item.filename == "IMAGES/boot.img" else src.read(item.filename))
    args.output_boot.write_bytes(updated)
    print(f"REFERENCE BOOT REPRODUCTION PASS: {BOOT_SHA}")
    print(f"NEW BOOT SHA256: {sha(updated)} ({len(updated)} bytes)")
    print(f"UPDATED TARGET-FILES: {args.output_target_files}")


if __name__ == "__main__":
    main()
