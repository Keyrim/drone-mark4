# mark4 mobile app

Flutter Android app, the phone-as-gateway node of the system: a BLE gamepad
paired to the phone, the phone on the drone's Wi-Fi speaking the transport.
`docs/mobile-app.md` holds what the two proofs of concept established, the
structure of the app and the roadmap; `docs/contributing/dart-guidelines.md`
how it is written.

What it does today: lists the drones heard on the network (name, node id),
connects to one and shows what it announced, and says when the drone is no
longer heard while staying on its page. The phone is one more node: kind
`PHONE`, its Announce named after the phone.

```sh
flutter pub get
./tool/gen.sh               # lib/gen/: protobuf codec, wire hash, ffigen binding
flutter analyze && dart format --set-exit-if-changed lib test && flutter test
../../scripts/adb_wifi.sh   # phone in wireless debugging, same Wi-Fi
flutter run                 # or: flutter build apk --debug --target-platform android-arm64
```

`native/` is the C++ side: `software/components/transport` compiled by the
NDK as it is (no copy, no source list) behind the C ABI of
`native/include/mark4/transport_shim.h`, which ffigen binds. Built by the
`mobile` CI job from the devcontainer image (Flutter, Android SDK, NDK,
libclang pinned in `.devcontainer/Dockerfile`).
