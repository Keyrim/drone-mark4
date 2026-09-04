# ota

The firmware update brick of every node that carries two firmware slots:
the flight controller, the desktop flight process (with files for slots)
and the ESP32 relay. It is a node brick like `log` and `transport`, not a
flight service: the platform layer is the abstract services of the flight
loop, and an update is what happens when the loop is parked. Header-only
INTERFACE target `ota`, `protocol/` alone underneath, no heap, no
iostream, no exceptions, so the same headers compile for the F405, for the
ESP32 (which builds `software/components/` sources as they are) and for the
desktop. `docs/ota-design.md` is the reference for every decision below.

## What is here

- `ota/firmware_store.hpp` - `AbsFirmwareStore`, the interface every
  updater drives: slot geometry, erase, program in order, read back, CRC,
  the boot metadata (`OtaMetaState`, read and append), plus the two image
  reads that depend on the image format of the chip: `imageValid()` and
  `readIdentity()`. Implementations stay with their targets:
  `platform_stm32/firmware_store_stm32.hpp` (internal flash),
  `platform_sim/firmware_store_sim.hpp` (files in a directory),
  `esp32-bridge/main/firmware_store_esp32.hpp` (ESP-IDF partitions).
- `ota/updater.hpp` - `OtaUpdater`, the board-side session: one `Ota*`
  message in, at most one reply out, the refusal policy, the go-back-N
  transfer, the staging record, the self-confirmation of a trial image on
  its first ground contact. Pure logic against the store, unit-tested on
  the desktop with fault injection (`software/tests/unit/test_ota_updater.cpp`).
- `ota/boot_policy.hpp` - the slot decision of a reset and the arming
  interlock, pure functions of an `OtaMetaState`. `drone_boot` and
  `drone_sim`'s fake trial boot run this very code.
- `ota/meta_log.hpp` - `OtaMetaLog`, the append-only record log over two
  erase-together areas: newest valid record wins, ping-pong when one area
  fills. Shared by the bootloader, the stm32 store and the sim store.
- `ota/image_header.hpp` - the `OtaImageHeader` reads of the two stores
  whose slots open with that header (stm32, sim): validity of a flashed
  image and its build identity.
- `ota/crc32_mpeg2.hpp` - the one checksum of the update system, software,
  bit for bit what the F405 hardware CRC unit computes over the same words.
  The hub uses this very code on the bundle.

## Two image formats behind one updater

An F405 slot opens with the 512-byte `OtaImageHeader` of
`protocol/ota_image.hpp`, stamped by `scripts/make_ota.py`. An ESP32 slot
holds a raw ESP-IDF application image: the IDF bootloader parses that
format and no other, so there is no `OtaImageHeader` to read there. The
updater therefore never reads an image itself. When a transfer completes it
asks the store whether the slot now holds an image of the announced size
built for this chip and this slot (`imageValid()`), and when it reports the
status it asks the store for each slot's build epoch and git hash
(`readIdentity()`). The stm32 and sim stores answer out of the header
prefix (`ota/image_header.hpp`); the esp32 store out of the IDF image
header and its `esp_app_desc_t`.

The metadata is the same story: the stm32 and sim stores persist
`OtaMetaState` through `OtaMetaLog`, the esp32 store synthesizes it from
what the IDF bootloader keeps in its `otadata` partition and translates
each write into the matching IDF call (set boot partition, cancel
rollback). The updater does not know which.
