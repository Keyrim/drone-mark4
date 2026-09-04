# Mobile app - phone as gateway

Status: two proofs of concept done (2026-09-02); first iteration of the app
in `software/mobile` (2026-09-03): the transport through ffigen, the drone
list and the drone page, the foundations below. Companion to `docs/tooling-architecture.md` (the hub and the
pages) and `docs/architecture.md` (the flight system).

## Why a phone

The drone is flown from a gamepad. Today the gamepad would have to reach a
laptop running the hub; a phone in the pilot's pocket removes the laptop
from the field: the gamepad pairs to the phone over Bluetooth LE, the phone
joins the drone's Wi-Fi and speaks the transport like every other node.

```
Xbox controller --BLE HID--> phone (Flutter app) --UDP transport--> ESP32 relay --UART--> board
```

Two questions had to be answered before writing any of it, one PoC each,
both throwaway apps kept out of this repository:

1. Can the existing C++ transport (`software/components/transport`) run
   inside a Flutter Android app, or must it be rewritten in Dart?
   (issue #12)
2. What does the phone add in latency between a BLE gamepad and a UDP
   datagram? (issue #9)

## PoC 1 - the C++ transport through dart:ffi (issue #12)

Result: the transport sources compile for Android with the NDK
**unmodified**. `transport.cpp`, `posix/udp_link.cpp`, `posix/node_id.cpp`
and the headers were copied byte for byte and built by Gradle's
`externalNativeBuild` (SDK cmake, `-std=c++20 -fno-exceptions -fno-rtti`,
no warning). Bionic has every call the link uses: `socket`, `bind`,
`setsockopt(SO_REUSEADDR | SO_REUSEPORT | SO_BROADCAST)`,
`recvfrom(MSG_DONTWAIT | MSG_TRUNC)`, `getsockname`, `getifaddrs`,
`/dev/urandom`. The phone appeared in the hub's node table on the first run.

What it took, and what the real implementation keeps:

- `minSdk 24`: `getifaddrs()` exists from API 24.
- A C ABI shim around `Transport` + `UdpLink` (create / destroy / poll /
  send / nodes / stats), opaque handle, **no callback across the FFI
  boundary**: the C++ side accumulates, Dart polls. Callbacks into Dart
  from native code are possible but tie the native thread to the Dart
  isolate; polling keeps the shim trivial and the threading model
  explicit.
- A Wi-Fi `MulticastLock` (Kotlin, one `MethodChannel`) plus the
  `ACCESS_WIFI_STATE` and `CHANGE_WIFI_MULTICAST_STATE` permissions:
  without it Android silently drops incoming broadcast datagrams, and the
  node hears nobody.
- The beacon is an `Envelope{announce}`: the PoC hand-encoded the 29 bytes
  in C++ (kind `BATCH`, no `NodeKind` exists for a phone yet) with the wire
  hash copied as a constant. The real app generates the Dart codec of
  `mark4.proto` at build time, as the pages do for TypeScript, and passes
  the beacon bytes to the shim.
- `flutter build apk --debug` ignores `abiFilters` and builds three ABIs;
  `--target-platform android-arm64` is the flag that restricts it.

Decision: **option 2, load the C++ transport through FFI**. One
implementation of the frame format, the node table, the beacon and the
relay rules, shared with the hub, the sim, the ESP32 and the board. The
Dart side owns what is application: the protobuf payloads (generated
codec) and the UI. Reimplementing the transport in Dart (option 1) buys
nothing the PoC showed a need for.

Open points carried into the roadmap:

- `poll()` ran on the UI isolate from a 5 ms timer. Its measured cost is
  the first number to read on the phone; the fallback is a native thread
  in the shim with a lock-free queue towards Dart, or a Dart isolate.
- Android drops the app's sockets and timers when it goes to the
  background; nothing was done about it. A foreground service is the
  usual answer for a live control link.
- Some access points block `255.255.255.255`; the link's loopback fallback
  flag tells.

## PoC 2 - Xbox controller over BLE, latency (issue #9)

Result: the path works and can be measured from the phone, with one
structural constraint: **an app cannot read the controller over GATT**.
Android reserves the HID service (UUID 0x1812) to the system, so there is
no `flutter_blue_plus` and no Bluetooth permission. The controller is paired
in the Android settings, the OS exposes it as an input device, and the app
reads `MotionEvent` / `KeyEvent` in Kotlin (`dispatchGenericMotionEvent`,
`dispatchKeyEvent` overrides of the `FlutterActivity`), forwarded to Dart
over an `EventChannel` as packed `Float64List`s. Flutter delivers no
joystick motion to Dart on its own.

What it took, and what the real implementation keeps:

- Gamepad events are **consumed** at the Activity (return `true`, BACK
  excepted). Left to Flutter, the hat becomes DPAD keys and drives the
  focus into whatever text field is on screen.
- `View.requestUnbufferedDispatch(SOURCE_JOYSTICK | SOURCE_GAMEPAD)`
  (API 30) turns off the batching of input to the display refresh; the
  PoC exposes it as a toggle so both behaviours are measurable.
  `MotionEvent.getHistorySize()` above zero means batching is on.
- Every timestamp lives in the `SystemClock.uptime` base: `eventTime`
  from the kernel, `uptimeNanos()` at the hook (API 34, else ms), and the
  Dart clock mapped onto it by a startup ping-pong keeping the round of
  smallest RTT.
- Haptics as a physical reference: a phone vibration must carry
  `VibrationAttributes.USAGE_ALARM`, or Android files it under touch
  feedback and drops it when that setting is off (`dumpsys
  vibrator_manager`: `ignored_for_settings`). The controller's own motor
  is reachable through `InputDevice.getVibratorManager()` and gives the
  full press -> phone -> BLE -> motor round trip in the hand.
- Xbox controller models: 1914 (Series X|S) and Elite Series 2 speak BLE
  out of the box; 1708 needs firmware 4.8.1902.0 or later; older ones
  cannot pair to a phone at all.

What the phone can and cannot see. The kernel timestamps a HID report when
the Bluetooth stack hands it over; everything before (stick sampling in
the controller, waiting for the next BLE connection event, the radio) is
invisible to the app and is typically the largest single term. The metrics
the PoC produces, each min / p50 / p95 / p99 over a rolling 500 samples:

| metric | what it covers |
|---|---|
| report interval | the HID report rate as timestamped by the kernel (BLE pad: ~8 ms expected) |
| d_dispatch | kernel timestamp -> app hook: input dispatcher, batching, scheduling |
| d_channel | the Kotlin -> Dart platform channel hop |
| historical samples / event | Android batching indicator (0 = none) |
| udp rtt | phone -> PC -> phone over Wi-Fi, no clock agreement needed |

Numbers from the first phone session are to be pasted here from the app's
CSV dump (`adb logcat -s flutter`, lines `POCXBOX,data,...`), with and
without unbuffered dispatch.

## The app

`software/mobile` is one Flutter project (`docs/contributing/dart-guidelines.md`
for the conventions). What the first iteration put in place:

- **The transport, unmodified, behind a C ABI.** `native/CMakeLists.txt` is
  the CMake project Gradle drives (`externalNativeBuild`), once per ABI. It
  includes `software/cmake/drone_targets.cmake` for the warning targets, sets
  `DRONE_PLATFORM` to `android` and adds `software/components/transport` as
  it is: the component's own CMakeLists.txt selects its POSIX sources for
  that value, and a file added to the component is built into the app with
  nothing to touch on this side. The only C++ of the app is
  `native/transport_shim.cpp`: one opaque handle holding a `UdpLink`, a
  `Transport` and a fixed ring of received payloads, and the flat functions
  of `native/include/mark4/transport_shim.h` (create, destroy, set beacon,
  send, poll, next payload, node table, counters). No callback crosses the
  boundary: the C++ side accumulates, Dart drains after each poll. The
  instant handed to `poll()` comes from a Dart `Stopwatch`; the C++ reads
  no clock, as everywhere.
- **Generated code only.** `tool/gen.sh` writes `lib/gen/` (gitignored): the
  Dart codec of `mark4.proto` (protoc from `grpcio-tools`, `protoc_plugin`
  from the pubspec, run with `dart run` like the pages run `protoc-gen-es`
  from node_modules), `wire_hash.dart` from the SHA-256 of the schema, and
  the `dart:ffi` binding of the shim header by ffigen (libclang from the
  LLVM of the image). No hand-written binding, no constant copied from C++.
- **The phone is a node.** `NodeKind.PHONE` in `mark4.proto`; the Announce
  is named after the phone (`Settings.Global.DEVICE_NAME`, the model as a
  fallback, cut to 16 ASCII characters), the node id is random at every
  launch like a desktop process. The hub and the pages show it as one more
  node of kind `phone`.
- **Back end / front end split.** `lib/back/` is the managers: `Backend`
  composes them and boots them in order (the C++ App pattern);
  `TransportManager` owns the node, its beacon, the poll timer (10 ms, UI
  isolate for now) and exposes the node table as snapshots; `DroneManager`
  derives the drones from the announced kinds and follows the one the user
  connected to (connected while it is in the table, lost while it is not,
  back on its own: the transport is connectionless, connecting is choosing
  an id); `SettingsManager` persists the theme mode. `lib/pages/` is one
  BLoC per page over those managers; `lib/widgets/` what pages share;
  `lib/theme/` every size and color, scaled by `flutter_screenutil` from a
  portrait design. The Android activity answers one method channel
  (`mark4/platform`: the Wi-Fi multicast lock, the device name), wrapped in
  `lib/back/platform/`.
- **Tests without a phone.** The managers run in `flutter test` over a fake
  transport node and a fake platform (`test/fakes/`): the test is the
  network, it puts nodes in the table and Envelopes in the queue and polls.
  The blocs are tested with `bloc_test` over the real managers.

Screens: the home page lists the drones (kind `FIRMWARE` or `DRONE_SIM`,
the same `isDrone` as the pages) with their name and id and counts the
other nodes; tapping one opens its page, which connects and shows a banner
(connected / disconnected / waiting) above what the drone announced and the
link counters. Portrait, few things per screen, large: the phone is set
down and the hands are on the controller.

## Roadmap of software/mobile

Done: the transport through ffigen with `NodeKind.PHONE`, the generated Dart
codec and wire hash, the drone list and the drone page (the first two steps
of the original roadmap, plus the foundations above). Next, each its own
pull request:

1. **Gamepad input module**: the Kotlin hooks of PoC 2 (consume, unbuffered
   dispatch on, history drained), one stream of stick / trigger / button
   samples towards Dart, plus a device list.
2. **Command path and cockpit**: the drone page becomes the cockpit. The
   wire and the flight core are ready for it (`Rc` carries the three
   sticks in the pilot's convention, the core has the `MANUAL` rate mode
   and the `LEVEL` leveling mode, `Status.rc_link_ok` says whether the
   drone hears its pilot, the fail-safe is 200 ms of silence). What the
   phone adds, gamepad in hand and nothing to touch on the screen:
   - `Rc` streamed at 50 Hz to the drone node id, sticks clamped and
     otherwise raw (deadband and ranges are the core's, tunable).
   - Layout (Xbox, mode 2): RT is the throttle in the direct-thrust modes
     (released = 0 = motors stopped, which is why a spring-loaded stick is
     not the throttle there); right stick roll and pitch; left stick X
     yaw; left stick Y reserved for the vertical velocity of altitude
     auto, where centre = hold has a physical meaning. B is the kill: one
     press, instant, latched by the phone until B is held for 1 s. A held
     for 1 s arms, only with RT at zero, sticks centred, drone connected,
     phase IDLE, IMU and baro valid; the missing condition is shown. A
     held again at zero throttle disarms. D-pad up/down selects the mode,
     while disarmed only. Menu and View stay the phone's.
   - Gamepad loss (`InputDeviceListener`, Bluetooth ACL disconnect), the
     app going to the background or the page being left send the safe
     state at once, then the stream stops.
   - Screen: a full-width phase band (grey IDLE, orange ARMED, green
     MANUAL / LEVEL, red FAULT and kill), three link indicators with
     their age (gamepad to phone, phone to drone, drone hears the phone
     from `rc_link_ok`), selected and locked mode side by side, the
     throttle gauge, four motor bars, a small horizon.
   - Haptics on the gamepad first, the phone (`USAGE_ALARM`) as fallback:
     double pulse armed, long buzz kill, repeated triple pulses while a
     link is lost, two short for a refused arm, one tick per mode change.
   Flown against `drone_sim` and the Godot plant before any motor turns.
   The transport `poll()` moves off the UI isolate before this step if
   PoC 1's numbers say so.
3. **Lifecycle**: foreground service while a drone is armed, sockets and
   the multicast lock survive the screen turning off, link status on
   screen.
4. **Latency budget in the repository**: the PoC 2 metrics as a debug
   screen of the real app, so every change to the input path is measured
   with the same numbers; the press -> buzz film test documented in
   `docs/bring-up.md` alongside the board procedures.
5. **Poll off the UI isolate** when the measured cost of the 10 ms timer
   says so: a native thread in the shim with a queue towards Dart, or a
   Dart isolate; the C ABI already accumulates and never calls back.
6. Later, not planned: iOS (the transport compiles, the input path and
   the BLE HID rules differ), telemetry views (the pages already do it;
   a phone screen is a different design), a network page or filters on the
   home page listing every node and not only the drones, a stable node id
   for the phone (a hash of `ANDROID_ID`) if the drone ever needs to
   recognise its pilot.
