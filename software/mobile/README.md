# mark4 mobile app

Flutter Android app, the phone-as-gateway node of the system: a BLE gamepad
paired to the phone, the phone on the drone's Wi-Fi speaking the transport.
Today a `flutter create` scaffold and nothing else; `docs/mobile-app.md`
holds what the two proofs of concept established and the roadmap.

```sh
../../scripts/adb_wifi.sh   # phone in wireless debugging, same Wi-Fi
flutter run                 # or: flutter build apk --debug --target-platform android-arm64
flutter analyze
```

Built by the `mobile` CI job from the devcontainer image (Flutter, Android
SDK, NDK pinned in `.devcontainer/Dockerfile`).
