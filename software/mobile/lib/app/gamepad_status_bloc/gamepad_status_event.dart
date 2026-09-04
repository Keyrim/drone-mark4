import 'package:equatable/equatable.dart';

sealed class GamepadStatusEvent extends Equatable {
  const GamepadStatusEvent();

  @override
  List<Object?> get props => [];
}

/// Start following the controller.
final class GamepadStatusStarted extends GamepadStatusEvent {
  const GamepadStatusStarted();
}

/// A controller appeared or left.
final class GamepadStatusChanged extends GamepadStatusEvent {
  const GamepadStatusChanged({required this.connected});

  final bool connected;

  @override
  List<Object?> get props => [connected];
}
