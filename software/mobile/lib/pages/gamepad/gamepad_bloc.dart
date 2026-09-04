import 'dart:async';

import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/gamepad/gamepad_manager.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';
import 'package:mark4/pages/gamepad/gamepad_event.dart';
import 'package:mark4/pages/gamepad/gamepad_state.dart';

export 'package:mark4/pages/gamepad/gamepad_event.dart';
export 'package:mark4/pages/gamepad/gamepad_state.dart';

/// The gamepad page: the controller as the manager sees it, and the two
/// haptic test buttons.
class GamepadBloc extends Bloc<GamepadPageEvent, GamepadPageState> {
  GamepadBloc(this._gamepad) : super(const GamepadPageState()) {
    on<GamepadPageStarted>(_onStarted);
    on<GamepadPageStateChanged>(
      (event, emit) => emit(state.copyWith(gamepad: event.gamepad)),
    );
    on<GamepadPageRumbleRequested>(_onRumble);
    on<GamepadPageVibrateRequested>(_onVibrate);
  }

  final GamepadManager _gamepad;
  StreamSubscription<GamepadState>? _subscription;

  Future<void> _onStarted(
    GamepadPageStarted event,
    Emitter<GamepadPageState> emit,
  ) async {
    await _subscription?.cancel();
    emit(state.copyWith(gamepad: _gamepad.state.value));
    _subscription = _gamepad.state
        .skip(1)
        .listen((gamepad) => add(GamepadPageStateChanged(gamepad)));
  }

  Future<void> _onRumble(
    GamepadPageRumbleRequested event,
    Emitter<GamepadPageState> emit,
  ) async {
    final done = await _gamepad.rumble();
    emit(
      state.copyWith(
        lastHaptic: done ? HapticOutcome.done : HapticOutcome.failed,
      ),
    );
  }

  Future<void> _onVibrate(
    GamepadPageVibrateRequested event,
    Emitter<GamepadPageState> emit,
  ) async {
    final done = await _gamepad.vibratePhone();
    emit(
      state.copyWith(
        lastHaptic: done ? HapticOutcome.done : HapticOutcome.failed,
      ),
    );
  }

  @override
  Future<void> close() async {
    await _subscription?.cancel();
    return super.close();
  }
}
