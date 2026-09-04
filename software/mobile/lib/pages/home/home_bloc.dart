import 'dart:async';

import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/gamepad/gamepad_manager.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/pages/home/home_event.dart';
import 'package:mark4/pages/home/home_state.dart';

export 'package:mark4/pages/home/home_event.dart';
export 'package:mark4/pages/home/home_state.dart';

/// The home page: the drones to connect to, who this phone is, and whether
/// a controller is in hand.
class HomeBloc extends Bloc<HomeEvent, HomeState> {
  HomeBloc({
    required this._drones,
    required this._transport,
    required this._gamepad,
  }) : super(const HomeState()) {
    on<HomeStarted>(_onStarted);
    on<HomeRosterChanged>(
      (event, emit) => emit(state.copyWith(roster: event.roster)),
    );
    on<HomeIdentityChanged>(
      (event, emit) => emit(state.copyWith(identity: event.identity)),
    );
    on<HomeGamepadChanged>(
      (event, emit) => emit(state.copyWith(gamepadConnected: event.connected)),
    );
  }

  final DroneManager _drones;
  final TransportManager _transport;
  final GamepadManager _gamepad;
  StreamSubscription<DroneRoster>? _rosterSubscription;
  StreamSubscription<TransportIdentity>? _identitySubscription;
  StreamSubscription<bool>? _gamepadSubscription;

  Future<void> _onStarted(HomeStarted event, Emitter<HomeState> emit) async {
    await _rosterSubscription?.cancel();
    await _identitySubscription?.cancel();
    await _gamepadSubscription?.cancel();
    emit(
      HomeState(
        roster: _drones.roster.value,
        identity: _transport.identity.value,
        gamepadConnected: _gamepad.state.value.connected,
      ),
    );
    _rosterSubscription = _drones.roster
        .skip(1)
        .listen((roster) => add(HomeRosterChanged(roster)));
    _identitySubscription = _transport.identity
        .skip(1)
        .listen((identity) => add(HomeIdentityChanged(identity)));
    _gamepadSubscription = _gamepad.state
        .skip(1)
        .map((state) => state.connected)
        .distinct()
        .listen((connected) => add(HomeGamepadChanged(connected: connected)));
  }

  @override
  Future<void> close() async {
    await _rosterSubscription?.cancel();
    await _identitySubscription?.cancel();
    await _gamepadSubscription?.cancel();
    return super.close();
  }
}
