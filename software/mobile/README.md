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
./tool/gen.sh               # lib/gen/: protobuf codec, wire hash
flutter analyze && dart format --set-exit-if-changed lib test && flutter test
../../scripts/adb_wifi.sh   # phone in wireless debugging, same Wi-Fi
flutter run                 # or: flutter build apk --debug --target-platform android-arm64
```

The app is pure Dart: it compiles no native code, and the communication
stack of the phone is written here, mirroring `software/components`
constant for constant. `lib/back/transport/` is the frame codec, the UDP
link and the node itself; `lib/back/messaging/` decodes one payload once
and dispatches it by `Envelope` body case; `lib/back/discovery/` answers
who the phone is and keeps a directory of who is around. Built by the
`mobile` CI job from the devcontainer image (Flutter and the Android SDK
pinned in `.devcontainer/Dockerfile`).
