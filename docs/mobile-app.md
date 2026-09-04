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
  (`mark4/platform`: the Wi-Fi multicast lock, the device name, the
  haptics), wrapped in `lib/back/platform/` and `lib/back/gamepad/`.
- **The gamepad, read at the activity** (2026-09-04). What PoC 2 found is
  now the app's `GamepadBridge.kt`: `dispatchGenericMotionEvent` and
  `dispatchKeyEvent` hand every gamepad-source event to the bridge and
  consume it (BACK excepted), the batched history is drained oldest first,
  `requestUnbufferedDispatch` is on for the gamepad sources from
  `onResume`, and an `InputManager.InputDeviceListener` reports the
  controllers coming and going. One `EventChannel` (`mark4/gamepad`)
  carries two kinds of events towards Dart: a report packed as a
  `Float64List` (device id, kernel timestamp in the uptime base, the two
  sticks, the two triggers, the hat, a button mask; the layout is spelled
  out once in the Kotlin constants and mirrored by `GamepadSample.fromPacked`)
  and the device list as a list of maps. Triggers are read from both the
  `BRAKE`/`GAS` and `LTRIGGER`/`RTRIGGER` axes (Xbox pads use the former),
  a hat reported as axes is folded into the four D-pad bits. Haptics are
  two methods of `mark4/platform`: `gamepadRumble` through the input
  device's vibrator manager, `vibratePhone` filed as `USAGE_ALARM` so the
  touch-feedback setting cannot silence it. On the Dart side
  `GamepadManager` exposes every report raw on `samples` (for whoever
  flies with them) and a throttled `state` (devices, last report, report
  rate) for the screens; a trailing timer publishes the last report a
  controller sent before going quiet, so released sticks show released.
- **The transmitter** (2026-09-04): `PilotManager` (`lib/back/pilot/`)
  turns the controller into the `Rc` stream of the connected drone, at
  50 Hz from the moment the cockpit opens (`engage()`) to the moment it
  closes (`disengage()`, the safe state twice, then silence). Every session
  starts killed. The gestures: B kills at once and latches, B held 1 s
  while killed clears it; A held 1 s arms, when the drone is IDLE with
  valid sensors and a fresh Status, RT is released and the sticks are
  centred (the missing condition is shown), A held again with RT released
  disarms; D-pad up / down cycles the mode while disarmed (`manual`,
  `level`, `altitude auto`), as does the segmented control on screen.
  RT is the throttle in the direct-thrust modes (released = motors
  stopped), the left stick's vertical axis is the throttle of altitude auto
  (centre = hold); right stick roll and pitch (forward = nose down), left
  stick yaw. A controller leaving kills at once, the app leaving the
  foreground kills, and a drone that dropped out of the armed phases while
  the phone still says armed (its fail-safe tripped, a cutoff) disarms the
  phone too, so flying again is a gesture and never a stream resuming.
  `DroneManager` decodes the `Status` broadcasts of the connected drone
  (`status`, throttled to 50 ms except on a phase change); the three links
  the cockpit shows are the controller's presence, the drone being heard,
  and `Status.rc_link_ok` fresh: the drone hears us. Haptic cues (armed,
  disarmed, killed, kill cleared, refused, mode, link lost every 2 s while
  it lasts, link back) go to the controller, or the phone when it has no
  rumble.
- **Tests without a phone.** The managers run in `flutter test` over a fake
  transport node and a fake platform (`test/fakes/`): the test is the
  network, it puts nodes in the table and Envelopes in the queue and polls.
  The blocs are tested with `bloc_test` over the real managers.

Screens: the home page lists the drones (kind `FIRMWARE` or `DRONE_SIM`,
the same `isDrone` as the pages) with their name and id and counts the
other nodes; tapping one opens its page, which connects and shows a banner
(connected / disconnected / waiting) above what the drone announced and the
link counters; since the transmitter it is the **cockpit**: a full-width
phase band in one word and one color (grey idle, orange armed, green
flying, red for killed, cutoff, fault, a lost drone or no Status), the
three link dots with their age, the mode selector, the gesture line (a
ring filling while A or B is held, the refusal reason, or the next thing
to do), the throttle gauge, a small horizon from the estimated attitude
next to the four motor bars, and the old details folded at the bottom.
The gamepad icon of the home app bar is lit while a
controller is present and opens the gamepad page: the controller's name,
the report count and rate, the two sticks drawn on their squares, the two
triggers as gauges, every button as a chip lit while down, and two buttons
to rumble the pad and buzz the phone. It is the page to open when a stick
feels wrong, before blaming the drone. Portrait, few things per screen,
large: the phone is set down and the hands are on the controller.

## Roadmap of software/mobile

Done: the transport through ffigen with `NodeKind.PHONE`, the generated Dart
codec and wire hash, the drone list and the drone page (the first two steps
of the original roadmap, plus the foundations above), the gamepad input
module (the Kotlin hooks of PoC 2, the event stream, the device list, the
haptics, the gamepad page), and the transmitter with the cockpit (the
`Rc` stream at 50 Hz, the gestures, the three links, the haptic cues; the
wire and the flight core carry the sticks, the `MANUAL` rate mode, the
`LEVEL` leveling mode and `Status.rc_link_ok`, the fail-safe is 200 ms).
To fly against `drone_sim` and the Godot plant before any motor turns.
Next, each its own pull request:

1. **Lifecycle**: foreground service while a drone is armed, sockets and
   the multicast lock survive the screen turning off, link status on
   screen.
2. **Latency budget in the repository**: the PoC 2 metrics as a debug
   screen of the real app, so every change to the input path is measured
   with the same numbers; the press -> buzz film test documented in
   `docs/bring-up.md` alongside the board procedures.
3. **Poll off the UI isolate** when the measured cost of the 10 ms timer
   says so: a native thread in the shim with a queue towards Dart, or a
   Dart isolate; the C ABI already accumulates and never calls back.
4. Later, not planned: iOS (the transport compiles, the input path and
   the BLE HID rules differ), telemetry views (the pages already do it;
   a phone screen is a different design), a network page or filters on the
   home page listing every node and not only the drones, a stable node id
   for the phone (a hash of `ANDROID_ID`) if the drone ever needs to
   recognise its pilot.
