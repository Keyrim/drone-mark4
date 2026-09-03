import 'dart:async';

import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/pages/drone/drone_event.dart';
import 'package:mark4/pages/drone/drone_state.dart';

export 'package:mark4/pages/drone/drone_event.dart';
export 'package:mark4/pages/drone/drone_state.dart';

/// The page of one drone: connects on start, disconnects when closed, shows
/// the link as the manager sees it.
class DroneBloc extends Bloc<DroneEvent, DroneState> {
  DroneBloc({required this._drones, required int nodeId})
    : super(DroneState(nodeId: nodeId)) {
    on<DroneStarted>(_onStarted);
    on<DroneConnectionChanged>(
      (event, emit) =>
          emit(DroneState(nodeId: state.nodeId, connection: event.connection)),
    );
  }

  final DroneManager _drones;
  StreamSubscription<DroneConnection>? _subscription;

  Future<void> _onStarted(DroneStarted event, Emitter<DroneState> emit) async {
    await _subscription?.cancel();
    _subscription = _drones.connection
        .where((connection) => connection.nodeId == state.nodeId)
        .listen((connection) => add(DroneConnectionChanged(connection)));
    await _drones.connect(state.nodeId);
  }

  @override
  Future<void> close() async {
    await _subscription?.cancel();
    await _drones.disconnect();
    return super.close();
  }
}
