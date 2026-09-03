# Mobile app - phone as gateway

Status: two proofs of concept done (2026-09-02), scaffold in `software/mobile`,
no feature yet. Companion to `docs/tooling-architecture.md` (the hub and the
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

## Roadmap of software/mobile

The scaffold is a `flutter create` and nothing else: it exists so the CI
builds an APK from the image on every push and the toolchain never drifts
silently. The steps, each its own pull request:

1. **Transport as a Flutter FFI plugin.** `software/mobile` gets a
   `CMakeLists.txt` that compiles `software/components/transport` from its
   real location (no copy; the Gradle externalNativeBuild path points at
   `../../components/transport`) plus the shim, and a hand-written
   `dart:ffi` binding. Beacon with a real `NodeKind`: add `PHONE` to
   `mark4.proto`. Hub and pages show the phone as one more node.
2. **Dart codec of mark4.proto** generated at build time (`protoc` +
   `protoc_plugin`, gitignored output like `software/hub/pages/src/gen`),
   the wire hash computed from the same source as CMake computes it, never
   a constant.
3. **Gamepad input module**: the Kotlin hooks of PoC 2 (consume, unbuffered
   dispatch on, history drained), one stream of stick / trigger / button
   samples towards Dart, plus a device list.
4. **Command path**: sticks -> `RcCommand` envelopes at the report rate,
   unicast to the drone node id; kill switch on a button, arming gesture,
   link-loss behaviour identical to what the firmware does when the
   transport goes silent. The transport `poll()` moves off the UI isolate
   before this step if PoC 1's numbers say so.
5. **Lifecycle**: foreground service while a drone is armed, sockets and
   the multicast lock survive the screen turning off, link status on
   screen.
6. **Latency budget in the repository**: the PoC 2 metrics as a debug
   screen of the real app, so every change to the input path is measured
   with the same numbers; the press -> buzz film test documented in
   `docs/bring-up.md` alongside the board procedures.
7. Later, not planned: iOS (the transport compiles, the input path and
   the BLE HID rules differ), telemetry views (the pages already do it;
   a phone screen is a different design).
