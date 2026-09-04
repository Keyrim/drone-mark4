import 'package:equatable/equatable.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

/// What the phone reports about game controllers, in the order it happens.
sealed class GamepadEvent extends Equatable {
  const GamepadEvent();

  @override
  List<Object?> get props => [];
}

/// The controllers present changed (sent once on subscription too).
final class GamepadDevicesEvent extends GamepadEvent {
  const GamepadDevicesEvent(this.devices);

  final List<GamepadDevice> devices;

  @override
  List<Object?> get props => [devices];
}

/// One report of a controller.
final class GamepadSampleEvent extends GamepadEvent {
  const GamepadSampleEvent(this.sample);

  final GamepadSample sample;

  @override
  List<Object?> get props => [sample];
}

/// The controllers as the operating system exposes them: an event stream
/// and the haptics. Implemented over the `mark4/gamepad` event channel and
/// the `mark4/platform` method channel of the Android activity; faked in
/// tests.
abstract class AbsGamepadSource {
  /// Device changes and reports, from the moment it is listened to.
  Stream<GamepadEvent> get events;

  /// Rumbles one controller; false when it cannot (gone, no vibrator).
  Future<bool> rumble(int deviceId, Duration duration, double amplitude);

  /// Vibrates the phone itself; false when it has no vibrator.
  Future<bool> vibratePhone(Duration duration);
}
