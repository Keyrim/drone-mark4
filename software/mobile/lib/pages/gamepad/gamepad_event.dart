import 'package:equatable/equatable.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

sealed class GamepadPageEvent extends Equatable {
  const GamepadPageEvent();

  @override
  List<Object?> get props => [];
}

/// Start following the controller.
final class GamepadPageStarted extends GamepadPageEvent {
  const GamepadPageStarted();
}

/// The manager's picture changed.
final class GamepadPageStateChanged extends GamepadPageEvent {
  const GamepadPageStateChanged(this.gamepad);

  final GamepadState gamepad;

  @override
  List<Object?> get props => [gamepad];
}

/// The user asked the controller to rumble.
final class GamepadPageRumbleRequested extends GamepadPageEvent {
  const GamepadPageRumbleRequested();
}

/// The user asked the phone to vibrate.
final class GamepadPageVibrateRequested extends GamepadPageEvent {
  const GamepadPageVibrateRequested();
}
