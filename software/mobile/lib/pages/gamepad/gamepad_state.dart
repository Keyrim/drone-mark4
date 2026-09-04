import 'package:equatable/equatable.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

/// Outcome of the last haptic request, for a one-line feedback.
enum HapticOutcome { none, done, failed }

final class GamepadPageState extends Equatable {
  const GamepadPageState({
    this.gamepad = GamepadState.empty,
    this.lastHaptic = HapticOutcome.none,
  });

  final GamepadState gamepad;
  final HapticOutcome lastHaptic;

  GamepadPageState copyWith({
    GamepadState? gamepad,
    HapticOutcome? lastHaptic,
  }) => GamepadPageState(
    gamepad: gamepad ?? this.gamepad,
    lastHaptic: lastHaptic ?? this.lastHaptic,
  );

  @override
  List<Object?> get props => [gamepad, lastHaptic];
}
