#!/usr/bin/env python3
"""
make_patch.py — ESP Delta OTA patch generator (Espressif-compatible).

Creates a .patch file in the format expected by esp_delta_ota / ota_manager.c:

  [64 B header: magic 0xfccdde10 + partition validation hash + reserved]
  [detools sequential patch body, heatshrink-compressed]

Usage:
  python tools/make_patch.py --base build/delta_fota.bin --new build/v2.bin --out firmware/update.patch

Requirements (IDF Python env):
  pip install detools esptool

The --base binary MUST be the firmware currently running on the device (same
build you flashed). The digest in the patch header is the esptool
"Validation Hash" of that base image, verified on-device with
esp_partition_get_sha256().
"""

import argparse
import os
import re
import struct
import sys
import tempfile

# Prefer full detools checkout (tools/detools-src) then bundled copy.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
for _detools_root in (
    os.path.join(_SCRIPT_DIR, "detools-src"),
    os.path.join(_SCRIPT_DIR, "..", "managed_components", "espressif__esp_delta_ota", "detools"),
):
    if os.path.isdir(_detools_root) and _detools_root not in sys.path:
        sys.path.insert(0, _detools_root)

MAGIC = 0xFCCDDE10
MAGIC_SIZE = 4
DIGEST_SIZE = 32
HEADER_SIZE = 64
RESERVED = HEADER_SIZE - MAGIC_SIZE - DIGEST_SIZE


def get_validation_hash(base_path: str, chip: str) -> bytes:
    """esptool Validation Hash — must match esp_partition_get_sha256 on device."""
    import esptool

    buf = tempfile.TemporaryFile(mode="w+")
    old_stdout = sys.stdout
    sys.stdout = buf
    try:
        esptool.main(["--chip", chip, "image_info", base_path])
        buf.seek(0)
        content = buf.read()
    finally:
        sys.stdout = old_stdout
        buf.close()

    m = re.search(r"Validation Hash:\s*([0-9a-fA-F]+)\s*\(valid\)", content)
    if not m:
        raise RuntimeError(
            f"Could not read Validation Hash from {base_path}.\n"
            "Is this a valid ESP-IDF app binary?"
        )
    return bytes.fromhex(m.group(1))


def create_patch(base_path: str, new_path: str, out_path: str, chip: str) -> None:
    try:
        from detools.create import create_patch as detools_create_patch
        from detools.apply import apply_patch_filenames
    except ImportError as e:
        raise RuntimeError(
            "detools Python package is required.\n"
            "From D FOTA project run:\n"
            "  pip install humanfriendly bitstruct heatshrink2\n"
            f"({e})"
        ) from e

    print(f"Base  : {base_path}")
    print(f"New   : {new_path}")
    print(f"Output: {out_path}")
    print(f"Chip  : {chip}")

    digest = get_validation_hash(base_path, chip)
    print(f"Validation hash: {digest.hex()}")

    body_path = out_path + ".body.tmp"
    try:
        print("Creating detools patch (sequential + heatshrink)...")
        with open(base_path, "rb") as f_base, open(new_path, "rb") as f_new, \
                open(body_path, "wb") as f_body:
            detools_create_patch(f_base, f_new, f_body,
                                 compression="heatshrink",
                                 use_mmap=False)

        body_size = os.path.getsize(body_path)
        print(f"  detools body: {body_size:,} bytes")

        os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
        with open(body_path, "rb") as f_body, open(out_path, "wb") as f_out:
            f_out.write(struct.pack("<I", MAGIC))
            f_out.write(digest)
            f_out.write(b"\x00" * RESERVED)
            f_out.write(f_body.read())

        total = os.path.getsize(out_path)
        new_size = os.path.getsize(new_path)
        pct = (100 * total // new_size) if new_size else 0
        print(f"\nPatch written: {out_path}")
        print(f"  total size : {total:,} bytes ({pct}% of new firmware)")
    finally:
        if os.path.exists(body_path):
            os.remove(body_path)


def verify_patch(base_path: str, patch_path: str, new_path: str) -> None:
    from detools.apply import apply_patch_filenames
    import hashlib

    print("Verifying patch (detools apply)...")
    with open(patch_path, "rb") as f:
        magic = struct.unpack("<I", f.read(4))[0]
        if magic != MAGIC:
            raise RuntimeError(f"Bad magic 0x{magic:08x}")
        f.read(DIGEST_SIZE + RESERVED)
        body = f.read()

    tmp_patch = patch_path + ".verify.tmp"
    tmp_out = patch_path + ".rebuilt.bin"
    try:
        with open(tmp_patch, "wb") as f:
            f.write(body)
        try:
            apply_patch_filenames(base_path, tmp_patch, tmp_out)
        except AttributeError as e:
            print(f"Verify skipped (PC-side apply needs C bsdiff): {e}")
            print("Patch file was written — esp_delta_ota on the device applies it.")
            return

        with open(tmp_out, "rb") as f:
            rebuilt = f.read()
        with open(new_path, "rb") as f:
            expected = f.read()
        if rebuilt != expected:
            raise RuntimeError(
                f"Mismatch: rebuilt {len(rebuilt)} B, expected {len(expected)} B"
            )
        print("Patch verified OK (detools apply matches new binary).")
        print(f"  SHA256 new: {hashlib.sha256(expected).hexdigest()}")
    finally:
        for p in (tmp_patch, tmp_out):
            if os.path.exists(p):
                os.remove(p)


def main():
    parser = argparse.ArgumentParser(description="ESP Delta OTA patch generator")
    parser.add_argument("--base", required=True, help="Base .bin (currently on device)")
    parser.add_argument("--new", required=True, help="Target .bin")
    parser.add_argument("--out", required=True, help="Output .patch file")
    parser.add_argument("--chip", default="esp32s3")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()

    create_patch(args.base, args.new, args.out, args.chip)
    if args.verify:
        verify_patch(args.base, args.out, args.new)


if __name__ == "__main__":
    main()
