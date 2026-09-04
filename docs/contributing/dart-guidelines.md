<!--
SPDX-FileCopyrightText: 2026 Théo Magne

SPDX-License-Identifier: MIT
-->

# Dart and Flutter guidelines <!-- omit in toc -->

How `software/mobile` is written. The hard rules of the root `CLAUDE.md`
apply as everywhere (English, ASCII, Conventional Commits, node ids as 8 hex
digits, no reference to the plan in code); `analysis_options.yaml` and `dart
format` are the enforced source of truth for what a tool can check, and CI
runs `flutter analyze`, `dart format --set-exit-if-changed` and
`flutter test`. This document is the rest.

## Layout

```
lib/
  main.dart      logging, Backend().init(), runApp
  app/           what sits above the pages: Mark4App, the router, the theme bloc
  theme/         colors, sizes, type scale, buildTheme(): every value the widgets use
  back/          the managers and what they expose; no widget, no material import
    manager.dart AbsManager
    backend.dart the composition root
    <area>/      one directory per manager: transport/, drone/, gamepad/, settings/, platform/
  pages/<page>/  one directory per page: <page>_page.dart, _bloc, _event, _state
  widgets/       widgets shared by several pages
  gen/           generated (gitignored): protobuf codec, wire hash, ffigen binding
native/          the C++ side: the C ABI shim and the CMake project Gradle drives
tool/            gen.sh and its helpers
test/            mirrors lib/ (back/, pages/), fakes in test/fakes/
```

## Back end: managers

- A manager owns one resource or one concern (`TransportManager`,
  `DroneManager`, `SettingsManager`) and extends `AbsManager`: `Future<bool>
  init()` acquires, logs its own failure and returns false; `Future<void>
  dispose()` releases. `Backend` constructs every manager, injects the
  dependencies by reference, calls `init()` in declaration order and
  `dispose()` in the reverse order, and stops at the first failure. This is
  the `App` class of the C++ executables.
- **Commands are synchronous methods returning a `Future`**:
  `Future<void> connect(int nodeId)`, `Future<bool> send(...)`. A caller
  awaits or not; a manager never exposes a callback to register.
- **State is exposed as a `ValueStream<T>` where `T extends Equatable`**
  (rxdart `BehaviorSubject`: the current value is readable at once with
  `.value`, every change is an event, a new listener gets the current value
  first). One subject per fact (`identity`, `snapshots`, `roster`,
  `connection`); the value type is immutable, and the manager adds a new
  value only when it differs from the current one. Nothing mutable is public.
- Events without a "current value" (a frame that arrived) are a plain
  `Stream`.
- `back/` imports no widget and no `package:flutter/material.dart`; a plugin
  (`shared_preferences`, a method channel) is fine, it is the phone, not the
  UI. The Dart side of the Android activity lives in `back/platform/`: what
  the phone provides as an operating system and Flutter does not expose.
- What a manager needs from below is an abstract class (`AbsTransportNode`,
  `AbsPlatform`) with the real implementation next to it, so a test runs the
  real managers over fakes (`test/fakes/`).

## Front end: BLoC

- One bloc per page (`pages/<page>/<page>_bloc.dart`) plus the app-level
  `ThemeBloc`. Events and states extend `Equatable`; events are a `sealed`
  class hierarchy so a `switch` is exhaustive.
- A bloc talks to managers and to nothing else: no manager is read from a
  widget, no widget holds logic. A bloc subscribes to manager streams on its
  start event and turns them into events of its own; it cancels its
  subscriptions in `close()`.
- Widgets read managers only to build a bloc (`context.read<DroneManager>()`
  in the page's `BlocProvider`), and render from `BlocBuilder` state.
- Pages are `go_router` routes (`app/router.dart`); a page is reached by its
  location, and a drone by its node id in the path.

## Theme and sizes

- `flutter_screenutil` scales from the portrait design size in
  `theme/app_theme.dart`. **No literal size, color or text style in a
  widget**: a size is a name in `AppSizes`, a color is the Material scheme or
  `AppColors`, a text style is the `TextTheme`.
- Light and dark are both built by `buildTheme(Brightness)`; the mode is a
  stored setting and every page's `Mark4AppBar` switches it.
- The rule of every screen: few things, large, readable from a distance. The
  phone is set down; the hands are on the controller.

## Logging

- `package:logging`: one `Logger` per file, hierarchical name in the
  project's style (`back/transport`, `app/boot`). The root listener is
  wired in `main.dart`. No `print`.

## Generated code

- `lib/gen/` is written by `tool/gen.sh` and gitignored: the Dart codec of
  `mark4.proto` (protoc from `grpcio-tools`, `protoc_plugin` from the
  pubspec), `wire_hash.dart` (the same first 8 hex characters of the SHA-256
  of the schema as CMake bakes into every C++ node), and the ffigen binding
  of `native/include/mark4/transport_shim.h`. Never write `dart:ffi`
  bindings by hand; a new native function is a line in the C header and a
  rerun of the script. A stale `lib/gen/` is the first thing to suspect
  when the app stops building after a schema change: the `mobile gen`
  VS Code task reruns the script (after `flutter pub get`), and the
  `mobile (flutter debug)` launch configuration of the workspace runs it
  before every debug session.
- The native side lists no source of the transport: `native/CMakeLists.txt`
  adds `software/components/transport` as it is, with `DRONE_PLATFORM`
  `android`.

## Naming

- Dart conventions (`UpperCamelCase` types, `lowerCamelCase` members,
  `snake_case` files). Abstract classes take the `Abs` prefix as in C++.
  Private fields are `_camelCase`; there is no `m_`.
- Doc comments are `///` above the declaration (Dart has no trailing form);
  Doxygen `@` tags are not used.
