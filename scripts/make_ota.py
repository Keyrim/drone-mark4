#!/usr/bin/env python3
"""Stamp the OTA image headers and package the update bundle.

The build links drone_firmware twice, once per flash slot; neither image
knows its own size, its checksum, its build time or the commit it came
from, so those five header fields are left at OTA_IMAGE_UNSTAMPED (0xFF)
by the firmware and filled in here. Stamping is what turns a linked image
into an image the bootloader verifies on every boot.

The build epoch is the identity of a build: unlike the git hash it tells
two packagings of the same (possibly dirty) tree apart, so it is what the
hub compares to decide whether the board already runs a bundle.

Produced next to the elfs:

    slot_a.img            slot A image, stamped, flashable at 0x08020000
    slot_b.img            slot B image, stamped, flashable at 0x08080000
    drone_firmware.ota    the bundle the hub sends: manifest + both images

    scripts/make_ota.py --slot-a build/.../drone_firmware_a.elf \\
                        --slot-b build/.../drone_firmware_b.elf \\
                        --outdir build/.../
    scripts/make_ota.py --verify build/.../drone_firmware.ota
"""

import argparse
import datetime
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OTA_HPP = os.path.join(
    REPO_ROOT, "software", "components", "protocol", "include", "protocol", "ota_image.hpp"
)

# OtaImageHeader field offsets, pinned by the static_asserts of
# software/components/protocol/include/protocol/ota_image.hpp. That header is
# the source of truth; these constants exist because a python tool cannot
# include it, and check_layout() checks them against it.
HEADER_SIZE = 512
OFF_MAGIC = 0
OFF_HEADER_VERSION = 4
OFF_MCU_ID = 6
OFF_SLOT_ID = 7
OFF_IMAGE_SIZE = 8
OFF_IMAGE_CRC = 12
OFF_BUILD_EPOCH = 16
OFF_GIT_HASH = 20
OFF_HEADER_CRC = 508
GIT_HASH_SIZE = 8

IMAGE_MAGIC = 0x5746344D  # "M4FW" read as a little-endian word
IMAGE_HEADER_VERSION = 1
MCU_STM32F405 = 1  # mark4.Mcu.STM32F405
UNSTAMPED = 0xFFFFFFFF
WIRE_HASH_SIZE = 8  # hex characters of the mark4.proto hash

# The .ota container, little-endian throughout: magic, then a length-prefixed
# ASCII JSON manifest, then one length-prefixed image per slot in slot order.
BUNDLE_MAGIC = b"M4OTA1\x00\x00"
SLOT_NAMES = ("a", "b")


def crc32_mpeg2(data: bytes) -> int:
    """CRC-32/MPEG-2 word-wise, the one checksum of the update system.

    Polynomial 0x04C11DB7, init 0xFFFFFFFF, no reflection, no final xor,
    consumed as 32-bit little-endian words with the tail padded to a word
    with 0xFF. It matches mark4::crc32Mpeg2 (components/ota) and the F405
    hardware CRC unit bit for bit. tools/telemetry_wire.py is the home of the
    Python wire codecs and holds the same function; this copy exists so the
    packaging step depends on nothing but the standard library.

    Vectors: crc32_mpeg2(bytes(4)) == 0xC704DD7B,
             crc32_mpeg2(bytes([1,2,3,4,5,6,7,8])) == 0xA3141BDA,
             crc32_mpeg2(bytes([1,2,3,4,5])) == 0xCCD0E62C.
    """
    data = data + b"\xff" * (-len(data) % 4)
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        crc ^= int.from_bytes(data[i:i + 4], "little")
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1) & 0xFFFFFFFF
    return crc


def read_cpp_constant(path: str, name: str) -> int:
    """Reads one `inline constexpr ... NAME = <number>` out of a C++ header."""
    with open(path, encoding="utf-8") as file:
        match = re.search(rf"\b{name}\s*=\s*(0[xX][0-9a-fA-F]+|\d+)", file.read())
    if match is None:
        print(f"make_ota: no constant '{name}' in {path}", file=sys.stderr)
        sys.exit(1)
    return int(match.group(1), 0)


def git_short_hash() -> str:
    """Short commit hash, GIT_HASH_SIZE hex characters.

    Falls back to zeros when git cannot answer (a source archive, or a CI
    container that does not own the checkout): the bundle is still valid, it
    just cannot name its commit, and the warning says so.
    """
    try:
        out = subprocess.run(
            ["git", "-C", REPO_ROOT, "rev-parse", f"--short={GIT_HASH_SIZE}", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        out = ""
    if len(out) != GIT_HASH_SIZE or any(c not in "0123456789abcdef" for c in out):
        print("make_ota: git rev-parse gave no usable hash, stamping zeros", file=sys.stderr)
        return "0" * GIT_HASH_SIZE
    return out


def to_binary(path: str, objcopy: str) -> bytes:
    """Returns the flash bytes of an image, running objcopy on an elf."""
    with open(path, "rb") as file:
        head = file.read(4)
    if head != b"\x7fELF":
        with open(path, "rb") as file:
            return file.read()
    with tempfile.TemporaryDirectory() as work:
        out = os.path.join(work, "image.bin")
        subprocess.run([objcopy, "-O", "binary", path, out], check=True)
        with open(out, "rb") as file:
            return file.read()


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def check_layout() -> None:
    """Guards these offsets against the pinned C++ header they mirror."""
    expected = {
        "OTA_IMAGE_MAGIC": IMAGE_MAGIC,
        "OTA_IMAGE_HEADER_VERSION": IMAGE_HEADER_VERSION,
        "OTA_IMAGE_HEADER_SIZE": HEADER_SIZE,
        "OTA_IMAGE_UNSTAMPED": UNSTAMPED,
        "OTA_GIT_HASH_SIZE": GIT_HASH_SIZE,
    }
    for name, value in expected.items():
        found = read_cpp_constant(OTA_HPP, name)
        if found != value:
            print(
                f"make_ota: {name} is {found:#x} in protocol/ota_image.hpp but {value:#x} here",
                file=sys.stderr,
            )
            sys.exit(1)


def stamp(image: bytes, slot: int, build_epoch: int, git_hash: str) -> bytes:
    """Fills in the five header fields only the packaging step can know.

    Refuses an image whose constant fields do not say what they must: a wrong
    magic means this is not an image at all, a wrong slot id means the build
    handed over the variants in the wrong order, and either one would ship a
    bundle the board rejects at the far end of a transfer.
    """
    if len(image) <= HEADER_SIZE:
        print(f"make_ota: slot {SLOT_NAMES[slot]} image is only {len(image)} bytes",
              file=sys.stderr)
        sys.exit(1)
    if u32(image, OFF_MAGIC) != IMAGE_MAGIC:
        print(f"make_ota: slot {SLOT_NAMES[slot]} image has no OTA image header "
              f"(magic {u32(image, OFF_MAGIC):#010x})", file=sys.stderr)
        sys.exit(1)
    header_version = struct.unpack_from("<H", image, OFF_HEADER_VERSION)[0]
    if header_version != IMAGE_HEADER_VERSION:
        print(f"make_ota: slot {SLOT_NAMES[slot]} image header version is {header_version}, "
              f"expected {IMAGE_HEADER_VERSION}", file=sys.stderr)
        sys.exit(1)
    if image[OFF_SLOT_ID] != slot:
        print(f"make_ota: image given as slot {SLOT_NAMES[slot]} was linked for slot id "
              f"{image[OFF_SLOT_ID]}", file=sys.stderr)
        sys.exit(1)
    if image[OFF_MCU_ID] != MCU_STM32F405:
        print(f"make_ota: slot {SLOT_NAMES[slot]} image targets mcu id {image[OFF_MCU_ID]}, "
              f"expected {MCU_STM32F405}", file=sys.stderr)
        sys.exit(1)

    out = bytearray(image)
    struct.pack_into("<I", out, OFF_IMAGE_SIZE, len(out))
    struct.pack_into("<I", out, OFF_IMAGE_CRC, crc32_mpeg2(bytes(out[HEADER_SIZE:])))
    struct.pack_into("<I", out, OFF_BUILD_EPOCH, build_epoch)
    out[OFF_GIT_HASH:OFF_GIT_HASH + GIT_HASH_SIZE] = git_hash.encode("ascii")
    struct.pack_into("<I", out, OFF_HEADER_CRC, crc32_mpeg2(bytes(out[:OFF_HEADER_CRC])))
    return bytes(out)


def verify_image(image: bytes, slot: int) -> bool:
    """Re-checks a stamped image exactly as the bootloader does."""
    ok = True
    size = u32(image, OFF_IMAGE_SIZE)
    if size != len(image):
        print(f"slot {SLOT_NAMES[slot]}: imageSize {size} but {len(image)} bytes", file=sys.stderr)
        ok = False
    if image[OFF_SLOT_ID] != slot:
        print(f"slot {SLOT_NAMES[slot]}: slotId {image[OFF_SLOT_ID]}", file=sys.stderr)
        ok = False
    image_crc = u32(image, OFF_IMAGE_CRC)
    computed = crc32_mpeg2(image[HEADER_SIZE:])
    if image_crc != computed:
        print(f"slot {SLOT_NAMES[slot]}: imageCrc {image_crc:#010x}, computed {computed:#010x}",
              file=sys.stderr)
        ok = False
    header_crc = u32(image, OFF_HEADER_CRC)
    computed = crc32_mpeg2(image[:OFF_HEADER_CRC])
    if header_crc != computed:
        print(f"slot {SLOT_NAMES[slot]}: headerCrc {header_crc:#010x}, computed {computed:#010x}",
              file=sys.stderr)
        ok = False
    return ok


def build_bundle(name: str, images: list, git_hash: str, wire_hash: str) -> bytes:
    """Serializes the pinned .ota container around a JSON manifest.

    wire_hash is the 8 hex characters the build computed from mark4.proto
    (DRONE_WIRE_HASH_HEX): the hub refuses a bundle built on another schema,
    since a board flashed with it would go silent on the bench.
    """
    first = images[0]
    manifest = {
        "name": name,
        "mcuId": first[OFF_MCU_ID],
        "buildEpoch": u32(first, OFF_BUILD_EPOCH),
        "gitHash": git_hash,
        "wireHash": wire_hash,
        "images": [
            # The manifest crc32 covers the WHOLE image, header included:
            # it is the value OtaBeginPacket announces and the value the
            # board checks the full slot against. The header's own
            # imageCrc field covers only the bytes after the header.
            {"slot": slot, "size": len(image), "crc32": crc32_mpeg2(image)}
            for slot, image in enumerate(images)
        ],
    }
    blob = json.dumps(manifest, separators=(",", ":"), sort_keys=True).encode("ascii")
    out = bytearray(BUNDLE_MAGIC)
    out += struct.pack("<I", len(blob))
    out += blob
    for image in images:
        out += struct.pack("<I", len(image))
        out += image
    return bytes(out)


def read_bundle(path: str) -> tuple:
    """Parses a bundle back into its manifest and images."""
    with open(path, "rb") as file:
        data = file.read()
    if not data.startswith(BUNDLE_MAGIC):
        print(f"make_ota: {path} is not an ota bundle", file=sys.stderr)
        sys.exit(1)
    cursor = len(BUNDLE_MAGIC)
    length = u32(data, cursor)
    cursor += 4
    manifest = json.loads(data[cursor:cursor + length].decode("ascii"))
    cursor += length
    images = []
    while cursor < len(data):
        length = u32(data, cursor)
        cursor += 4
        images.append(data[cursor:cursor + length])
        cursor += length
    return manifest, images


def verify_bundle(path: str) -> int:
    """Re-reads a bundle and re-verifies every checksum it claims."""
    manifest, images = read_bundle(path)
    ok = len(images) == len(SLOT_NAMES)
    if not ok:
        print(f"make_ota: bundle holds {len(images)} images, expected {len(SLOT_NAMES)}",
              file=sys.stderr)
    for slot, image in enumerate(images):
        ok = verify_image(image, slot) and ok
        declared = manifest["images"][slot]
        if declared["size"] != len(image) or declared["crc32"] != crc32_mpeg2(image):
            print(f"make_ota: manifest disagrees with the slot {SLOT_NAMES[slot]} image",
                  file=sys.stderr)
            ok = False
    if not ok:
        return 1
    built = datetime.datetime.fromtimestamp(manifest["buildEpoch"], datetime.timezone.utc)
    print(f"make_ota: {os.path.basename(path)} verified - {manifest['name']} "
          f"built {built:%Y-%m-%d %H:%M:%S} UTC "
          f"({manifest['gitHash']}), wire {manifest['wireHash']}, "
          f"slots {', '.join(str(len(image)) + ' B' for image in images)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--slot-a", help="slot A image, elf or raw binary")
    parser.add_argument("--slot-b", help="slot B image, elf or raw binary")
    parser.add_argument("--outdir", help="directory the artifacts are written to")
    parser.add_argument("--name", default="drone_firmware", help="bundle name in the manifest")
    parser.add_argument("--git-hash", help=f"{GIT_HASH_SIZE} hex characters, default: git HEAD")
    parser.add_argument("--wire-hash",
                        help=f"{WIRE_HASH_SIZE} hex characters of the mark4.proto hash the "
                             "images were built with (the build passes DRONE_WIRE_HASH_HEX)")
    parser.add_argument("--build-epoch", type=int,
                        help="unix seconds stamped as the build identity, default: now")
    parser.add_argument("--objcopy", default="arm-none-eabi-objcopy",
                        help="objcopy used to turn an elf into flash bytes")
    parser.add_argument("--verify", help="re-read a bundle and re-check every checksum")
    args = parser.parse_args()

    if args.verify:
        return verify_bundle(args.verify)
    if not (args.slot_a and args.slot_b and args.outdir and args.wire_hash):
        parser.error("--slot-a, --slot-b, --outdir and --wire-hash are required")
    if len(args.wire_hash) != WIRE_HASH_SIZE:
        parser.error(f"--wire-hash must be {WIRE_HASH_SIZE} characters")

    check_layout()
    git_hash = args.git_hash or git_short_hash()
    if len(git_hash) != GIT_HASH_SIZE:
        parser.error(f"--git-hash must be {GIT_HASH_SIZE} characters")
    build_epoch = args.build_epoch if args.build_epoch is not None else int(time.time())
    if not 0 <= build_epoch < UNSTAMPED:
        parser.error("--build-epoch must fit an unsigned 32-bit word, below the 0xFF sentinel")

    images = [
        stamp(to_binary(args.slot_a, args.objcopy), 0, build_epoch, git_hash),
        stamp(to_binary(args.slot_b, args.objcopy), 1, build_epoch, git_hash),
    ]
    os.makedirs(args.outdir, exist_ok=True)
    for slot, image in enumerate(images):
        path = os.path.join(args.outdir, f"slot_{SLOT_NAMES[slot]}.img")
        with open(path, "wb") as file:
            file.write(image)
    bundle = os.path.join(args.outdir, f"{args.name}.ota")
    with open(bundle, "wb") as file:
        file.write(build_bundle(args.name, images, git_hash, args.wire_hash))

    for slot, image in enumerate(images):
        print(f"make_ota: slot {SLOT_NAMES[slot].upper()} {len(image)} bytes, "
              f"crc {u32(image, OFF_IMAGE_CRC):#010x}")
    print(f"make_ota: {bundle} ({os.path.getsize(bundle)} bytes, git {git_hash})")
    return verify_bundle(bundle)


if __name__ == "__main__":
    sys.exit(main())
