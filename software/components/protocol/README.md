# protocol

The wire of the project: one schema, `mark4.proto`, and the codecs the
build generates from it. Every datagram and every serial frame between the
flight processes, the board, the plant and the ground tools carries exactly
one `Envelope`, a `oneof` over every message of the system (telemetry, the
lockstep sensor and actuator frames, RC, announce, log, scenario and run
stats, tuning, updater). Nothing generated is committed.

| Consumer | Generator | Output | When |
|----------|-----------|--------|------|
| every C/C++ target, desktop and STM32 | nanopb (FetchContent, `nanopb-0.4.9.2`, `PB_NO_MALLOC`, `PB_BUFFER_ONLY`, `PB_NO_ERRMSG` on the board) | `software/build/<preset>/gen/nanopb/mark4.pb.{c,h}` | the `nanopb` target, on every build |
| the Godot plant | godobuf, the addon committed in `sim-godot/addons/godobuf/` (pinned commit, BSD-3) run by a headless Godot through `scripts/gen_godobuf.py` | `sim-godot/scripts/gen/mark4.gd` and `wire_hash.gd` (gitignored) | target `proto_gd` of the desktop preset, when `godot` is on the PATH |
| the batch tool | `python3 -m grpc_tools.protoc --python_out` | `software/build/desktop/gen/python/mark4_pb2.py` and `mark4_wire_hash.py` | target `proto_py` of the desktop preset |

`mark4.options` bounds every field for nanopb (string sizes, fixed-count
float vectors, the 240-byte chunk, two OTA slots) so the structs hold their
storage inline, and gives every enum a fixed 32-bit underlying type so an
unknown value from a newer peer is a plain integer to compare, never an
out-of-range enum load. The generator needs the python packages `protobuf`
and `grpcio-tools` (the devcontainer image ships them); the configure step
says so when they are missing.

## What the C++ side gets

- `protocol/envelope.hpp`: `mark4.pb.h` plus `encodeEnvelope()` /
  `decodeEnvelope()`, buffer in, buffer out, no allocation, and
  `MAX_ENVELOPE_SIZE` (259 bytes today: the full OTA chunk). Static asserts
  keep `sizeof(mark4_Envelope)` under 400 bytes, and the transport and
  serial framing check that every envelope fits their payloads.
- `protocol/wire_hash.hpp`: `WIRE_HASH`, the first 8 hex characters of the
  SHA-256 of `mark4.proto`, computed by CMake at configure time and
  regenerated on every edit of the schema. Every `Announce` carries it; the
  hub compares it with its own and exposes `wireMismatch` per node in its
  `discovery` JSON, the console shell paints the chip red. The packaging
  script stamps it into the `.ota` manifest (`wireHash`) and the hub
  refuses a bundle built on another schema. The Godot plant and the batch
  tool read theirs from `wire_hash.gd` / `mark4_wire_hash.py`.
- `protocol/ota_image.hpp`: what is not wire but still crosses processes:
  the on-flash `OtaImageHeader`, the slot and chip identities in their
  flash encoding (`OTA_SLOT_*`, `OTA_MCU_*`, EMPTY is 0xFF on flash and 0
  on the wire, `otaSlotStateToWire()` maps), the chunk size and window.
Enum values are C-scoped inside the package, so two enums never share a
value name: `PHASE_*`, `THROW_*`, `RC_*`, `OTA_OK`, `OTA_OP_*` carry the
prefix the clash forced, the rest stay short. The flight core's own enums
(FlightPhase, ThrowState, PilotMode, TuningStatus) are pinned to the wire
value by value in `platform_common/telemetry_packer.hpp` and
`tuning_service.hpp`; flight-core never includes this library.

## Where the messages travel

- `Telemetry`, `SimRunStats`, `TuningAck`, `TuningInfo`, the `Ota*`
  answers and the `Announce` beacon: flight process to ground, as transport
  broadcasts (drone_sim, and the board through the ESP32 relay).
  `Telemetry.truth` is the plant's exact state when the sender has one; the
  hub renders it as the `simRaw` JSON message.
- `Rc`, `Reboot`, `SimScenario`, `Tuning{Set,Get,List}`, the `Ota*`
  requests: ground to flight process, as transport unicasts or serial
  frames.
- `SimSensor` (truth included) and `SimActuator`: the lockstep exchange
  between a flight process and its plant, transport unicasts between the
  two node ids; `SimScenario` is forwarded to the plant the same way as
  its own envelope, once per scenario, and the plant plays it once per
  change of `sequence`.

The serial framing (`transport/serial_framing.hpp`) carries one envelope
per frame behind a two-byte length, 512 bytes at most like the transport's
`MAX_PAYLOAD`.

## Changing the schema

Edit `mark4.proto` (and `mark4.options` when a bound moves), rebuild: the
three codecs regenerate, the wire hash changes, and every node built before
shows up as a `wireMismatch` in the hub instead of a silent bench. A board
running the previous schema still has to be reflashed once over SWD (or
updated by a hub built on the previous schema): see `docs/ota-design.md`.
The C++ unit tests round-trip every message
(`software/tests/unit/test_protocol_envelope.cpp`) and one test spawns a
headless Godot to exchange envelopes with the generated GDScript codec
through the two transports (`test_plant_link.cpp`, skipped without a godot
binary).
