import 'dart:async';

import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/pages/home/home_event.dart';
import 'package:mark4/pages/home/home_state.dart';

export 'package:mark4/pages/home/home_event.dart';
export 'package:mark4/pages/home/home_state.dart';

/// The home page: the drones to connect to, and who this phone is.
class HomeBloc extends Bloc<HomeEvent, HomeState> {
  HomeBloc({required this._drones, required this._transport})
    : super(const HomeState()) {
    on<HomeStarted>(_onStarted);
    on<HomeRosterChanged>(
      (event, emit) => emit(state.copyWith(roster: event.roster)),
    );
    on<HomeIdentityChanged>(
      (event, emit) => emit(state.copyWith(identity: event.identity)),
    );
  }

  final DroneManager _drones;
  final TransportManager _transport;
  StreamSubscription<DroneRoster>? _rosterSubscription;
  StreamSubscription<TransportIdentity>? _identitySubscription;

  Future<void> _onStarted(HomeStarted event, Emitter<HomeState> emit) async {
    await _rosterSubscription?.cancel();
    await _identitySubscription?.cancel();
    emit(
      HomeState(
        roster: _drones.roster.value,
        identity: _transport.identity.value,
      ),
    );
    _rosterSubscription = _drones.roster
        .skip(1)
        .listen((roster) => add(HomeRosterChanged(roster)));
    _identitySubscription = _transport.identity
        .skip(1)
        .listen((identity) => add(HomeIdentityChanged(identity)));
  }

  @override
  Future<void> close() async {
    await _rosterSubscription?.cancel();
    await _identitySubscription?.cancel();
    return super.close();
  }
}
