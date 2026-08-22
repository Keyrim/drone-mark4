#!/usr/bin/env bash
# Flash the mark1 flight controller (STM32F405RG) over SWD with a SEGGER
# J-Link. This is the only path that writes the bootloader, and the recovery
# path of last resort when both OTA slots are dead.
#
# THE FLASH MAP THIS SCRIPT WRITES (docs/ota-design.md section 4.2)
#
#   0x08000000  32 KB   sectors 0-1   drone_boot.bin      (bootloader)
#   0x08008000  32 KB   sectors 2-3   boot metadata        (ping-pong pair)
#   0x08010000  64 KB   sector 4      reserved
#   0x08020000 384 KB   sectors 5-7   slot_a.img           (firmware slot A)
#   0x08080000 384 KB   sectors 8-10  slot_b.img           (firmware slot B)
#   0x080A0000 128 KB   sector 11     spare
#
# SINCE THE FIRMWARE IS ALWAYS A SLOT IMAGE
#
# There is no longer a firmware linked at 0x08000000: every firmware elf is
# linked for a slot, and the bootloader is what the reset vector reaches.
# For a normal SWD debug session, flash the bootloader once and then work
# with drone_firmware_a.elf - it is the image at slot A, its debug symbols
# match the addresses the core executes, and the bootloader jumps to it as
# long as the boot metadata says slot A (which "install" and "meta-wipe" both
# arrange). drone_firmware_b.elf is only for bench experiments on the trial
# boot and rollback path.
#
# WHAT EACH SUBCOMMAND TOUCHES, AND NOTHING ELSE
#
#   install    sectors 0-3 erased, then boot + slot A written, then reset.
#              The fresh-board command: it also wipes the metadata, so the
#              board starts from the defaults (slot A active and trusted).
#   boot       sectors 0-1 only. Slots and metadata survive.
#   slot-a     0x08020000 only. The bootloader, the metadata and slot B
#              survive. Note this does NOT change the metadata: if the board
#              currently prefers slot B, it will keep booting slot B.
#   slot-b     0x08080000 only, for trial-boot and rollback experiments.
#   meta-wipe  sectors 2-3 only: the board falls back to the defaults of
#              OtaMetaState (slot A active and VALID, slot B empty). The way
#              out of a board stuck in a rollback loop.
#   erase      the whole chip. Everything above is gone, bootloader included.
#
# Nothing here writes a slot while the board runs from it: the probe halts
# the core first, so that hazard belongs to the OTA path, not to this script.
#
#   scripts/flash_stm32.sh install
#   scripts/flash_stm32.sh boot | slot-a | slot-b | meta-wipe | erase
#   scripts/flash_stm32.sh install --build-dir software/build/stm32
#
# Build the artifacts first: python3 scripts/build_app.py drone_firmware and
# cmake --build --preset stm32 --target drone_boot (the stm32 preset builds
# both by default).
set -euo pipefail

JLINK="${JLINK:-/opt/SEGGER/JLink/JLinkExe}"
DEVICE="STM32F405RG"
INTERFACE="SWD"
SPEED_KHZ=4000

BOOT_ADDRESS=0x08000000
META_START=0x08008000
META_END=0x0800FFFF
SLOT_A_ADDRESS=0x08020000
SLOT_B_ADDRESS=0x08080000

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${repo_root}/software/build/stm32"
command=""

usage() {
    sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        install | boot | slot-a | slot-b | meta-wipe | erase)
            command="$1"
            shift
            ;;
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "flash_stm32: unknown argument '$1'" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "${command}" ]]; then
    usage >&2
    exit 1
fi

if [[ ! -x "${JLINK}" ]]; then
    echo "flash_stm32: no JLinkExe at ${JLINK} (override with JLINK=/path/to/JLinkExe)" >&2
    exit 1
fi

boot_bin="${build_dir}/drone_boot/drone_boot.bin"
slot_a_img="${build_dir}/drone_firmware/slot_a.img"
slot_b_img="${build_dir}/drone_firmware/slot_b.img"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "flash_stm32: missing $1" >&2
        echo "flash_stm32: build it first (cmake --build --preset stm32)" >&2
        exit 1
    fi
}

# Builds the J-Link command file for the chosen subcommand and runs it. The
# script is printed before it runs: on a board this is the last chance to
# notice that an address is wrong.
stage_dir="$(mktemp -d -t flash_stm32.XXXXXX)"
script_file="${stage_dir}/commands.jlink"
trap 'rm -rf "${stage_dir}"' EXIT

# JLinkExe decides the file format from the extension and rejects ".img",
# so slot images are staged as ".bin" copies before loadbin sees them.
stage_bin() {
    local staged="${stage_dir}/$(basename "${1%.img}").bin"
    cp "$1" "${staged}"
    echo "${staged}"
}

{
    echo "si ${INTERFACE}"
    echo "speed ${SPEED_KHZ}"
    echo "connect"
    # Halt before touching flash: the core must not be executing out of a
    # region that is about to be erased.
    echo "halt"

    case "${command}" in
        install)
            require_file "${boot_bin}"
            require_file "${slot_a_img}"
            # Sectors 0-3: the bootloader and both metadata areas. The slots
            # are not erased here; loadbin overwrites what it covers, and a
            # stale slot B is harmless because the metadata says slot A.
            echo "erase ${BOOT_ADDRESS} ${META_END}"
            echo "loadbin ${boot_bin} ${BOOT_ADDRESS}"
            echo "loadbin $(stage_bin "${slot_a_img}") ${SLOT_A_ADDRESS}"
            ;;
        boot)
            require_file "${boot_bin}"
            echo "loadbin ${boot_bin} ${BOOT_ADDRESS}"
            ;;
        slot-a)
            require_file "${slot_a_img}"
            echo "loadbin $(stage_bin "${slot_a_img}") ${SLOT_A_ADDRESS}"
            ;;
        slot-b)
            require_file "${slot_b_img}"
            echo "loadbin $(stage_bin "${slot_b_img}") ${SLOT_B_ADDRESS}"
            ;;
        meta-wipe)
            echo "erase ${META_START} ${META_END}"
            ;;
        erase)
            echo "erase"
            ;;
    esac

    echo "r"
    echo "go"
    echo "qc"
} >"${script_file}"

echo "flash_stm32: ${command} on ${DEVICE} at ${SPEED_KHZ} kHz"
echo "--- J-Link script ---"
cat "${script_file}"
echo "---------------------"

"${JLINK}" -device "${DEVICE}" -if "${INTERFACE}" -speed "${SPEED_KHZ}" \
    -autoconnect 1 -NoGui 1 -ExitOnError 1 -CommandFile "${script_file}"
