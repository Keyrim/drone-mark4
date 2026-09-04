import 'dart:async';

import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/app/gamepad_status_bloc/gamepad_status_event.dart';
import 'package:mark4/app/gamepad_status_bloc/gamepad_status_state.dart';
import 'package:mark4/back/gamepad/gamepad_manager.dart';

export 'package:mark4/app/gamepad_status_bloc/gamepad_status_event.dart';
export 'package:mark4/app/gamepad_status_bloc/gamepad_status_state.dart';

/// Whether a controller is in hand: a fact of the whole app, not of one
/// page, so it lives above the router and every app bar shows it.
class GamepadStatusBloc extends Bloc<GamepadStatusEvent, GamepadStatusState> {
  GamepadStatusBloc(this._gamepad) : super(const GamepadStatusState()) {
    on<GamepadStatusStarted>(_onStarted);
    on<GamepadStatusChanged>(
      (event, emit) => emit(GamepadStatusState(connected: event.connected)),
    );
  }

  final GamepadManager _gamepad;
  StreamSubscription<bool>? _subscription;

  Future<void> _onStarted(
    GamepadStatusStarted event,
    Emitter<GamepadStatusState> emit,
  ) async {
    await _subscription?.cancel();
    _subscription = _gamepad.state
        .map((state) => state.connected)
        .distinct()
        .listen((connected) => add(GamepadStatusChanged(connected: connected)));
  }

  @override
  Future<void> close() async {
    await _subscription?.cancel();
    return super.close();
  }
}
