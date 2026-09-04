# OTA firmware update - design proposal

Status: implemented (the Ota* messages of mark4.proto, protocol/ota_image.hpp,
the components/ota brick, the stm32, sim and esp32 firmware stores,
drone_boot, scripts/make_ota.py in both modes, hub OtaClient and the update
panel; the desktop end-to-end test drives a full update and a rollback
against drone_sim; the relay of section 8 updates over the air from the same
panel). This document remains the reference for the design decisions.
Companions: `docs/target-architecture.md` (system picture this plugs into),
`docs/bring-up.md` (mark1 board), `docs/aio-board-spec.md` (F722 AIO board).

## 1. Goal and scope

Reflash `drone_firmware` from the hub, over the radio path that already
exists, with no cable and no debug probe, and with the guarantee that no
event during the update - power cut, lost link, corrupted transfer, or a
new image that does not boot - ever leaves the board unable to run the
previous firmware. The guarantee comes from a dual-bank layout: two
firmware slots in flash, the running one never touched by the update path,
a small bootloader that picks between them.

In scope: the STM32 flight firmware, on both boards (mark1 F405 today,
AIO F722 next). Out of scope for v1, listed with reasons in section 7:
updating the bootloader itself, updating the ESP32 relay, image signing,
resuming an interrupted transfer, delta updates.

## 2. The path the bytes take, and what it imposes

```
hub (desktop) --UDP/WiFi--> ESP32 relay --UART 921600--> flight controller
```

- **The relay never looks inside.** The ESP32 is a transport relay
  (`esp32-bridge/`): the hub sends wire envelopes as transport unicasts
  to the board's node, they land at the relay's address, the relay
  forwards them down the UART in the serial framing (transport header,
  then the envelope) and the board's `UartLink` picks them up; the
  board's answers are broadcasts the relay forwards onto the LAN. The
  updater messages are unicasts for the board like RC is, so the relay's
  filter (which only keeps LAN broadcasts off the UART) never sees them.
  The board is a transport node like `drone_sim`; the hub has no special
  path for it. OTA needs no relay change.
- **The serial framing caps a frame at 512 payload bytes** (two length
  bytes) and discards corrupted frames by CRC-16. WiFi loses datagrams.
  The transfer protocol must therefore retransmit, and must pace itself:
  the receiver's acknowledgements are the flow control that keeps the hub
  from overrunning the relay's UART transmit side (the UART is the
  bottleneck at roughly 92 KB/s; the relay refuses a frame its transmit
  ring cannot hold whole and counts it).
- **Flash on both MCUs is single-bank at the silicon level**: programming
  or erasing stalls every code fetch from flash. So the update runs in a
  dedicated update mode with the flight loop stopped, and UART reception
  must sit on a DMA circular buffer, which keeps capturing while the CPU
  is stalled.
- **Two chips, one logic.** mark1 carries an STM32F405 (1 MB flash); the
  AIO board an STM32F722 (512 KB flash, plus a 16 MB W25Q128 NOR). The
  slot geometry is per-chip data; every
  state machine, packet and tool is shared.

## 3. Functional proposal

### 3.1 A nominal update, as the operator sees it

1. **Build.** `python3 scripts/build_app.py drone_firmware` produces, next
   to the elf, one `drone_firmware.ota` bundle: the image linked for slot
   A, the image linked for slot B, and a manifest (build epoch, git
   hash, target MCU). The build epoch is the identity of a build: unlike
   the git hash it tells two packagings of the same dirty tree apart.
2. **Connect.** The drone sits on the bench, disarmed, relay powered. The
   hub control page already shows the board; it now also shows the running
   build and which slot it runs from (one status request).
3. **Start.** The operator picks the bundle (the hub offers the most
   recent build by default) and clicks update. The hub checks the manifest
   against the board (right MCU, right protocol version) and shows what is
   about to replace what.
4. **Transfer.** The board erases the inactive slot (a few seconds, shown
   as its own phase), then the image streams down with a progress bar. At
   the end the board CRC-checks the full slot against the announced CRC
   and stages the image. Order of magnitude: a 128 KB image transfers in
   two to three seconds; erase dominates on the F405 (up to ~6 s for a
   384 KB slot).
5. **Trial boot.** The hub sends the existing reboot command. The
   bootloader sees a staged image and boots it exactly once.
6. **Confirm.** The trial image confirms itself on the first ground
   request it serves: by then it has booted, run its loop and proven the
   receive side of the radio, which is everything the next update needs.
   The hub polls status anyway, so this takes seconds and no gesture.
   The new slot becomes the active one; the old image stays in the other
   slot as the rollback target.
7. **Verdict.** The page states plainly which firmware the board runs and
   whether the update confirmed or rolled back. Total nominal time: well
   under half a minute.

### 3.2 Safety interlocks

- An update request is **refused while armed**, and update mode keeps the
  motors unconditionally off (no DShot output at all; the ESCs observe
  silence and disarm). The kill-switch semantics of normal operation are
  untouched because normal operation is suspended, not modified.
- An update request is **refused under a battery-voltage floor**: the
  process survives power loss by design, but a trial boot on a dying
  battery would waste the single trial attempt for nothing.
- **Arming is refused while the running image is unconfirmed** (trial
  boot). A firmware that has not yet proven its link cannot take the
  drone into the air.

### 3.3 Failure behavior - the point of dual bank

| Event | Outcome |
| --- | --- |
| Power cut during erase or transfer | The running slot was never touched; the board reboots into the old firmware. The half-written slot is not staged, so the bootloader ignores it. |
| Corrupted or incomplete transfer | The final CRC check fails, the image is not staged, the old firmware keeps running. The hub reports it and the operator retries. |
| Link lost mid-transfer | The board times out the session, discards it and returns to normal mode. The hub reports the abort. |
| New image boots but crashes, bootloops, or never talks | It was booted as a one-shot trial. On the next reset (watchdog or power cycle) the bootloader sees a trial that was attempted and not confirmed, marks the slot bad and boots the old firmware. |
| New image is fine but the operator regrets it | A revert command activates the other slot (still valid) and reboots. |
| Both slots invalid | Unreachable through the update path: the updater may only ever erase the slot that is not running. The floor below everything remains SWD, exactly as today. |

The invariant behind the whole table: **the running image and the
bootloader are never written by the update path, and every state change is
one atomic metadata record**. Power can drop at any byte of the process
and the board still boots something known-good.

## 4. Dual bank: layout and boot logic

### 4.1 Two execute-in-place slots, no copy

Two strategies exist for dual bank on parts without hardware bank
swapping:

- **Execute in place (chosen)**: two slots, each holding a complete image
  linked for its own address; the bootloader verifies and jumps to the
  active one. No copy step, so staging an image is instant, and rollback
  is a metadata flip. Cost: the build links the firmware twice, and the
  hub must send the variant matching the inactive slot (the board tells
  it which, and the image header lets the board refuse a mismatch).
- **Staging plus copy**: one execution address, a staging area (internal
  or on the AIO board's NOR), the bootloader copies after verifying.
  Single binary, bigger maximum image, but the boot path gains a copy
  window and the bootloader gains the NOR driver.

Chosen: execute in place. It is the smallest bootloader, the fewest
moving parts at boot time, and the current firmware (21 KB of flash
today) fits a 128 KB slot six times over. The escape hatch is real and
cheap: **the slot strategy is invisible on the wire and in the hub** - if
the F722 image ever outgrows 128 KB, the bootloader alone changes to a
copy-from-NOR scheme and nothing else moves.

### 4.2 Flash layouts

STM32F405 (mark1, 1 MB):

| Sectors | Size | Content |
| --- | --- | --- |
| 0-1 | 32 KB | bootloader (`drone_boot`), SWD-flashed, never updated OTA |
| 2-3 | 2 x 16 KB | boot metadata, ping-pong pair |
| 4 | 64 KB | reserved (future on-board parameter persistence) |
| 5-7 | 384 KB | slot A |
| 8-10 | 384 KB | slot B |
| 11 | 128 KB | spare |

STM32F722 (AIO board, 512 KB):

| Sectors | Size | Content |
| --- | --- | --- |
| 0-1 | 32 KB | bootloader |
| 2-3 | 2 x 16 KB | boot metadata, ping-pong pair |
| 4 | 64 KB | reserved (future parameter persistence) |
| 5 | 128 KB | slot A |
| 6 | 128 KB | slot B |
| 7 | 128 KB | spare (or the growth path of section 4.1) |

### 4.3 Image format

An image is the slot content, byte for byte: a 512-byte header, then the
vector table and code. 512 keeps the vector table alignment valid on both
cores. Header fields, all fixed-size, packed, little-endian like every
wire struct:

| Field | Purpose |
| --- | --- |
| magic, header version | reject garbage and stale layouts |
| image size, image CRC-32 | what the bootloader verifies before every jump |
| target MCU id, target slot | the board refuses an image built for the wrong chip or the wrong slot |
| build epoch, git hash | what the hub and the announce of section 5 report; the epoch is the identity the hub verdict compares |

CRC-32 is **CRC-32/MPEG-2 computed word-wise** (polynomial 0x04C11DB7,
init 0xFFFFFFFF, no reflection, no final xor, images padded to 4 bytes):
that is the one configuration the F405 hardware CRC unit can compute, so
the check on every boot costs milliseconds, and every other party (F722,
hub, packaging script) can reproduce it.

### 4.4 Boot metadata

The two 16 KB metadata sectors hold an append-only sequence of fixed-size
records: monotonic counter, active slot, per-slot state, trial-attempt
flag, CRC-32. The latest record with a valid CRC wins; a torn write
(power cut mid-record) fails its CRC and the previous record stands -
this is what makes every state transition atomic. When one sector fills,
the next record opens the other sector and the full one is erased:
ping-pong, so there is never a moment without a valid record.

Both the bootloader and the firmware write records (the bootloader to
mark trials and bad slots, the firmware to stage and confirm), through
one shared metadata module.

### 4.5 Slot states and the trial boot

```mermaid
stateDiagram-v2
    EMPTY --> STAGED: transfer complete,\nCRC verified (firmware)
    STAGED --> TESTING: one-shot trial\n(bootloader, marks attempted)
    TESTING --> VALID: first ground request served\n(firmware self-confirm)
    TESTING --> BAD: reset without confirm\n(bootloader boots the old slot)
    VALID --> EMPTY: erased as the next\nupdate target
    BAD --> EMPTY: erased as the next\nupdate target
```

The bootloader's whole job at reset: read the latest metadata record; if
a slot is STAGED, mark it TESTING with the attempted flag set and boot it;
if a slot is TESTING and attempted, mark it BAD and boot the active slot;
otherwise CRC-check the active slot and jump. Any image failing its CRC
is treated as BAD on the spot. If nothing bootable remains (flash decay,
not the update path), the bootloader signals on the status LED and waits
for SWD.

Confirmation is the firmware vouching for itself, the classic pattern of
MCUboot and ESP-IDF, but anchored to ground contact rather than a bare
init checkpoint: serving one valid request proves boot, loop and radio
reception, so an image whose receive path is broken never confirms and
rolls back on the next reset. What self-confirmation gives up is the
transmit-side proof only the ground could observe; the trade buys a
confirmation that needs no session state anywhere.

## 5. Wire protocol

The updater messages are bodies of the one `Envelope` of `mark4.proto`
(`software/components/protocol/`), like everything else on the wire. All
session messages carry a 32-bit session nonce chosen by the hub at
`OtaBegin`, so duplicates from an abandoned session cannot corrupt a new
one.

| Message | Direction | Content and role |
| --- | --- | --- |
| OtaStatusRequest | hub to board | ask for the update status |
| OtaStatus | board to hub | mcu, running and active slot, then per slot the state and the image identity (build epoch, git hash), slot size, max chunk size; the hub uses it to pick the image variant, to read the trial verdict and to paint the slot table |
| OtaBegin | hub to board | session nonce, image size, image CRC-32; refused while armed or under the voltage floor. The board erases the inactive slot, then acks; the hub allows that ack a generous timeout, erase is seconds long |
| OtaChunk | hub to board | session, byte offset, up to 240 data bytes (the largest envelope of the wire, 259 bytes encoded, inside the 512-byte framing) |
| OtaChunkAck | board to hub | session, next expected offset; sent every 16 chunks and on any out-of-order chunk |
| OtaFinish | hub to board | session; the board CRC-checks the whole slot and writes the STAGED record |
| OtaRevert | hub to board | activate the other slot if VALID; followed by the existing reboot command |
| OtaAbort | hub to board | drop the session, return to normal mode |
| OtaAck | board to hub | one answer for begin, finish, revert and abort: the acknowledged operation (`OtaOp`), the session, and `OTA_OK` or a refusal reason (`OtaResult`) |

The trial image confirms itself on its first ground contact; there is no
confirm message any more.

Transfer discipline: the board writes chunks strictly in order and its
ack is cumulative (next expected offset), classic go-back-N. A lost or
corrupted chunk simply makes the acks repeat the same offset; the hub
resends from there after a short timeout. Sixteen chunks in flight is
about 3.8 KB, comfortably inside one UART DMA buffer, and keeps the link
saturated: the effective rate stays near the 92 KB/s ceiling. Selective
retransmission is not worth its board-side bookkeeping at these sizes.

Reboot reuses the existing `Reboot` message; nothing else in the wire
moves. Godot never sees these messages (they exist only between hub and
board); the codecs are generated from the one schema, so nothing has to be
kept in step by hand.

```mermaid
sequenceDiagram
    participant H as hub
    participant B as board (firmware)
    participant L as bootloader
    H->>B: OtaStatusRequest
    B->>H: OtaStatus (old build, slot A active)
    H->>B: OtaBegin (size, crc, session)
    Note over B: erase slot B (seconds)
    B->>H: OtaAck (BEGIN, OTA_OK)
    loop windows of 16 chunks
        H->>B: OtaChunk x16
        B->>H: OtaChunkAck (next offset)
    end
    H->>B: OtaFinish
    Note over B: CRC slot B, write STAGED
    B->>H: OtaAck (FINISH, OTA_OK)
    H->>B: Reboot
    Note over L: STAGED found, mark TESTING\nattempted, boot slot B
    H->>B: OtaStatusRequest
    Note over B: first ground contact:\nwrite VALID record (self-confirm)
    B->>H: OtaStatus (new build, slot B, VALID)
```

### 5.1 What a schema change costs

There is no version byte: the wire is whatever `mark4.proto` says, and
every build computes a 32-bit hash of that file (`WIRE_HASH`) that travels
in its `Announce`. Protobuf tolerates added fields, but a change to an
existing field or to the framing strands the board that is already
flashed: its envelopes decode into nonsense or not at all, and the hub
drops what it cannot use. The one thing that stays visible is the
mismatch itself: the hub logs every node whose hash differs from its own
("speaks wire ..., this hub speaks ...") and publishes both hashes
(`GatewayStatus.wire_hash`, `Node.announce.wire_hash` in the `NodeTable`),
which the console shell compares and paints red on the node's chip. The
old hub refuses the new bundle on
the `wireHash` check in `ota_bundle.cpp`, so nothing is broken and nothing
works: the board cannot be trusted by the new hub, nor updated by the old
one.

There are two ways out. The blunt one is a single SWD flash of the new
firmware, which needs the board on the bench. The other is a temporary
migration hub, built from the commit the board runs (same `mark4.proto`,
hence same hash) with the bundle hash check bypassed: it speaks the old
wire while pushing the new-wire image, and the update is an ordinary OTA
exchange from there. The trial slot protects it as usual - if the new image
never reaches ground contact, the auto-revert puts the old wire back.

A schema change is therefore a bench operation, not a field one. Plan it
when the board is reachable, and flash before shipping a hub that no longer
speaks to it.

## 6. Software architecture

The pieces, placed by the dependency rule (flight-core depends on
nothing; platform speaks protocol; humans speak to the hub):

- **`mark4.proto`** (the `Ota*` messages and enums of section 5) and
  **`protocol/ota_image.hpp`** - the image header of section 4.3 and the
  slot-state bytes as the flash stores them. The image header lives in a
  header of its own because it crosses a process boundary without being
  wire (packaging script writes it, bootloader and hub read it); the
  packaging tool carries a Python-side encoder checked against the C++
  constants.
- **`AbsFirmwareStore`** (components/ota; formerly a platform header,
  moved per section 8.1) - slot geometry, erase slot, program in order,
  read back, compute CRC, read and append metadata records.
  Implementations stay with their targets: `stm32` (internal flash
  driver plus the hardware CRC unit), `sim` (file-backed slots in the
  run directory), and `esp32` (section 8.3). flight-core never sees it.
- **`OtaUpdater`** (components/ota; formerly platform_common, moved
  per section 8.1) - the board-side session state
  machine: consumes OTA packets, drives an AbsFirmwareStore, emits
  replies. Pure logic against an abstract store, so it unit-tests on
  desktop with fault injection: lost chunks, duplicates, stale sessions,
  a store that dies mid-write to simulate power loss. This is where the
  guarantees of section 3.3 become Catch2 tests instead of promises.
- **`drone_boot`** - a new, fourth flight-side executable, and the one
  composition that contains no flight-core at all: startup, metadata
  read, the state decisions of section 4.5, CRC check, vector-table jump.
  It shares the flash and metadata modules with platform_stm32, speaks no
  UART in v1, and must stay well inside its 32 KB. Flashed over SWD once
  per board, then left alone; its interface to the rest of the world is
  the metadata format and nothing else, which is what makes the strategy
  swap of section 4.1 possible later.
- **`drone_firmware`** - FirmwareApp gains an update mode: on an accepted
  OTA_BEGIN it parks the flight loop, silences the motors, and runs a
  minimal loop that feeds frames to the OtaUpdater and the watchdog
  (refreshed before each sector erase; erase stalls the core but the
  IWDG runs on its own clock and the UART DMA keeps filling RAM).
  On OTA_ABORT, session timeout or OTA_FINISH it returns to normal mode.
- **`drone_sim`** - grows the same OtaUpdater over the file-backed store.
  Not to update the sim (it is rebuilt, not flashed) but because it makes
  the entire flow - hub service, page, protocol, state machine -
  exercisable end to end with zero hardware, and CI-able. Fake trial
  boots included: the sim store flips its metadata exactly like the real
  bootloader would.
- **Build and packaging** - the linker script is parameterized by slot
  base and length; the stm32 preset links `drone_boot`,
  `drone_firmware_a` and `drone_firmware_b`. A packaging step
  (`scripts/make_ota.py`) computes the CRC, stamps the header on both
  binaries and emits the `.ota` bundle; `build_app.py` and `apps.json`
  pick it up so one command produces the flashable artifact.
- **Hub** - an OtaClient service: opens the bundle, matches it against
  OTA_STATUS, runs the windowed transfer with retries and timeouts,
  drives reboot, watches the board come back and reads the verdict off
  the status (the trial image confirms itself on first contact), exposes
  the whole thing over the existing WebSocket as the `OtaCommand` /
  `OtaState` messages of `gateway.proto`: a command names the target node,
  the state is republished on every change.
- **Pages** - an update panel on the control page: running build
  against bundle build, phase and progress, the confirmed-or-rolled-back
  verdict, a revert button.
- **ESP32 relay** - no change, by construction.

Verification, in the spirit of everything else in this repo: unit tests
on OtaUpdater and the metadata module (torn-record recovery above all), a
round trip of every updater message through the generated codec, and one
desktop end-to-end test in CI - hub OtaClient against drone_sim's updater
through real UDP, full update, then a simulated failed trial that must
roll back.

## 7. Deliberately not in v1

- **Image signing.** The CRC is integrity, not authenticity; the security
  boundary today is the WPA2 network the relay raises or joins. Worth
  revisiting the day the drone updates over anything but its own bench
  network; the header reserves room for it.
- **Transfer resume.** A restart costs a few seconds; resume costs state
  that survives a session, on both ends, for nothing measurable.
- **Bootloader recovery over UART.** It would double the bootloader to
  cover a state (both slots dead) the update path cannot produce. SWD is
  the recovery of last resort, exactly as it is today.
- **Bootloader self-update.** An SWD flash on the bench today, and it
  never blocks the drone loop. (ESP32 relay OTA sat in this list in v1;
  it is designed in section 8 now.)
- **Delta updates and compression.** At 128 KB over a 92 KB/s link, the
  problem they solve does not exist here.

## 8. The relay updates over the same wire (ESP32 OTA)

The v1 flow above updates the F405 from the pages with no probe. The
relay riding the drone is the last node still asking for a USB cable,
and everything the flow needs already exists for it: the `Ota*`
messages name no chip (`OtaStatus.mcu` says which one is answering,
`Mcu.ESP32C3` exists, and the hub already refuses a bundle whose mcu
does not match the board), the transfer is transport unicasts and the
relay is a transport node, and ESP-IDF ships a dual-slot bootloader
with rollback - the same shape as sections 4.4 and 4.5, implemented by
someone else. The relay updates over UDP directly, so the 92 KB/s UART
budget of section 2 does not even apply to it.

### 8.1 One OTA brick: components/ota

The device-side OTA code first lived in the platform library
(`platform/firmware_store.hpp`, `platform_common/ota_updater.hpp`,
`ota_boot_policy.hpp`, `ota_meta_log.hpp`, `crc32_mpeg2.hpp`). That
placement was convenience, not design: the platform layer is defined as
the abstract services of the flight loop, and OTA is not one - it is a
node brick, like `log` and `transport`, wanted by the firmware, the
desktop flight process, the hub (which keeps its `OtaClient` outside
platform already, having no platform at all) and the relay, which
links `components/` sources only and has no reason to start including
platform headers for a non-flight concern.

So the brick lives in **`software/components/ota/`**: `AbsFirmwareStore`
and the `OtaMetaState`/`OtaMetaRecord` types, `OtaUpdater`, the boot
policy, the metadata log, and the CRC-32/MPEG-2 helper (which the hub
uses too, instead of a private copy). Header-only INTERFACE target
`ota`, depending on `protocol` alone; include paths keep the module
prefix (`#include "ota/updater.hpp"`). The store implementations did not
move: they are genuinely target-specific and stay with their targets
(`platform_stm32`, `platform_sim`, and `esp32-bridge/main/`). A
mechanical move for the existing code paths - same headers, new address
- plus the interface growth of section 8.2.

### 8.2 What the generic updater stopped assuming

`OtaUpdater` used to read the flashed image twice, and both reads
assumed the F405 image format of section 4.3:

- **finish validation**: after the CRC pass, it reads the
  `OtaImageHeader` prefix at the slot base and checks magic, header
  version, mcu, slot id and image size;
- **status identity**: `OtaStatus` reports each slot's `buildEpoch` and
  `gitHash` out of that same header.

An ESP32 slot holds a raw ESP-IDF image (the IDF bootloader parses
that format and no other), so there is no `OtaImageHeader` to read.
Both reads are `AbsFirmwareStore` virtuals - `imageValid()` (slot,
expected size) and `readIdentity()` (build epoch, git hash). The stm32
and sim stores implement them with the exact header-prefix logic the
updater used to hold, shared in `ota/image_header.hpp`, so F405 wire
behavior did not change by a byte; the esp32 store implements them
against the IDF image format (IDF's own image verification, the
`esp_app_desc_t`). `protocol/ota_image.hpp` is untouched.

### 8.3 The esp32 store over ESP-IDF

The bridge left the single-app partition table for a two-OTA layout
(`esp32-bridge/partitions.csv`): `ota_0`/`ota_1` (slots A/B, 1984 KB
each on the 4 MB flash of the module) plus the `otadata` entry the IDF
bootloader reads, and `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` so a new
image boots exactly once as pending-verify - IDF's name for the one-shot
trial of section 4.5. `drone_boot` and the metadata log are NOT compiled
on the ESP32: the trial/rollback equivalence lives in each chip's
bootloader, and `FirmwareStoreEsp32` translates between `OtaMetaState`
and what IDF exposes:

| Store call | ESP-IDF |
|---|---|
| `runningSlot()` | `esp_ota_get_running_partition()` subtype |
| `slotSize()` | the partition's size |
| `mcuId()` | `ESP32C3` |
| `eraseSlot()` | `esp_partition_erase_range()` in 64 KB blocks, yielding between two so the idle task keeps feeding the task watchdog |
| `program()` | `esp_partition_write()` |
| `read()` | `esp_partition_read()` |
| `crc32()` | `esp_partition_read()` + the shared software CRC |
| `imageValid()` | `esp_image_verify()` over the slot, then its `image_len` against the announced size |
| `readIdentity()` | `esp_ota_get_partition_description()`, the version string parsed back |
| `readMeta()` | synthesized: `activeSlot` from `esp_ota_get_boot_partition()`; per-slot states from `esp_ota_get_state_partition()` (VALID from img-valid, TESTING from pending-verify, BAD from invalid/aborted, STAGED from new; a slot IDF has no opinion about, the USB-flashed image or the one a rollback went back to, is VALID when it holds an application and EMPTY otherwise); `trialAttempted` is true while the running image is pending-verify |
| `writeMeta()` | by diff against `readMeta()`: target newly STAGED -> `esp_ota_set_boot_partition(target)`; running slot newly VALID (the self-confirm of section 3.1) -> `esp_ota_mark_app_valid_cancel_rollback()`; `activeSlot` moved to the other valid slot (revert) -> `esp_ota_set_boot_partition(other)`; marking a slot EMPTY before its erase -> a no-op, unless that slot was the boot partition (a staged image nobody rebooted into), in which case the boot goes back to the running slot |

Slot identity (section 8.2) comes from `esp_app_desc_t`: the build
stamps `PROJECT_VER` as `<buildEpoch>-<gitHash>` at configure time (the
two values the top-level `CMakeLists.txt` computes for the Announce and
hands to the packaging step), and the store parses it back. 32 bytes of
version field fit it with room to spare.

One behavior to accept, not fix: `eraseSlot()` occupies the relay's main
task for the seconds a partition erase takes, so the relay stops
relaying and UART bytes fall on the floor for that window. That is the
section 3 philosophy verbatim - an accepted update session parks normal
operation - and the hub's begin timeout already allows for it. Updating
the relay mid-flight is refused by nobody but common sense; the drone
is on the ground when its radio gets reflashed.

The first flash of the two-slot layout is a USB one (`idf.py flash`
writes the partition table, `otadata` and `ota_0`): a relay still on the
single-app layout has no `otadata` for the bootloader to read and no
second slot to write, and says so at boot (`app/boot` ERROR) while
relaying as before.

### 8.4 Bundle and ground side

`scripts/make_ota.py` has an `--esp32` mode: the manifest names
`mcuId: ESP32C3` and carries **one image instead of two** - an IDF app
image is position-independent across OTA partitions, so the same bytes
program either slot, where the F405 links one binary per slot. The
bundle container is unchanged (magic, JSON manifest, length-prefixed
images); the manifest says how many images follow (`imageCount`), and
the hub's bundle reader picks the image for the target slot as
`min(slot, imageCount - 1)`. No header stamping: the image is the raw
`.bin` the IDF build produced, and the bundle CRC is computed over it
as-is; the one check the ground makes on it is the IDF magic byte, the
relay verifies the rest with IDF's own code before staging. The bridge
build packages `esp32_bridge.ota` next to its image, with the build epoch
and git hash it stamped as `PROJECT_VER`, so the identity the bundle
announces is the one the relay will report.

The hub `OtaClient` and the pages changed for nothing else: the OTA
panel targets a node id, the relay is a node, and the mcu match
refusal already existed. The operator types the bundle path
(`esp32-bridge/build/esp32_bridge.ota`) in the panel's path field, the
default being the flight controller's bundle.

### 8.5 Relay composition

`relay.cpp` already decoded the unicasts addressed to the relay (the
`LogControl` path); the updater rides the same spot: the body tag is
read off the first bytes (`envelopeBodyTag()`, the `Ota*` tags do not
fit the one-byte compare the `LogControl` check used), decode, offer to
`OtaUpdater::handle()` first (exactly like the firmware's command
drain), send whatever reply comes back to the sender, plus the `Reboot`
envelope answered with `esp_restart()`. `Inputs` are simple on a radio:
never armed, no battery floor, `esp_timer` as the clock. The updater
keeps its defaults - self-confirm on first ground contact included,
which the store maps onto IDF's rollback cancel. The relay logs the
session under `relay/ota`, the store under `ota/store`.

Verification keeps the section 6 spirit: the updater's unit tests moved
with it unchanged (they run against a fake store); the esp32 store is a
thin translation exercised on the hardware unit; the desktop end-to-end
CI test is untouched, and the hub's bundle tests cover the one-image
manifest.
