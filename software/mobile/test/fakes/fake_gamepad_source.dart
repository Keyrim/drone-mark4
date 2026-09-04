import 'dart:async';

import 'package:mark4/back/gamepad/abs_gamepad_source.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

/// A controller in a test: the test plugs devices in, sends reports and
/// reads back what haptics were asked.
class FakeGamepadSource implements AbsGamepadSource {
  FakeGamepadSource({this.rumbleWorks = true, this.phoneVibrates = true});

  final StreamController<GamepadEvent> _events = StreamController.broadcast();
  final bool rumbleWorks;
  final bool phoneVibrates;
  final List<(int deviceId, Duration duration, double amplitude)> rumbles = [];
  final List<Duration> phoneVibrations = [];

  @override
  Stream<GamepadEvent> get events => _events.stream;

  /// The controllers present, as the platform would report after a change.
  void devices(List<GamepadDevice> devices) =>
      _events.add(GamepadDevicesEvent(devices));

  /// One report.
  void report(GamepadSample sample) => _events.add(GamepadSampleEvent(sample));

  @override
  Future<bool> rumble(int deviceId, Duration duration, double amplitude) async {
    rumbles.add((deviceId, duration, amplitude));
    return rumbleWorks;
  }

  @override
  Future<bool> vibratePhone(Duration duration) async {
    phoneVibrations.add(duration);
    return phoneVibrates;
  }

  Future<void> close() => _events.close();
}
