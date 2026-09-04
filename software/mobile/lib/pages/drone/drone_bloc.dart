import 'dart:async';

import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/pilot/pilot_manager.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/pages/drone/drone_event.dart';
import 'package:mark4/pages/drone/drone_state.dart';

export 'package:mark4/pages/drone/drone_event.dart';
export 'package:mark4/pages/drone/drone_state.dart';

/// The page of one drone, which is the cockpit: connects and engages the
/// transmitter on start, disengages and disconnects when closed, shows the
/// link, the drone's Status and the transmitter as the managers see them.
class DroneBloc extends Bloc<DroneEvent, DroneState> {
  DroneBloc({
    required this._drones,
    required this._pilot,
    required this._transport,
    required int nodeId,
  }) : super(DroneState(nodeId: nodeId)) {
    on<DroneStarted>(_onStarted);
    on<DroneConnectionChanged>(
      (event, emit) => emit(state.copyWith(connection: event.connection)),
    );
    on<DroneStatusChanged>(
      (event, emit) => emit(
        state.copyWith(status: event.status, clearStatus: event.status == null),
      ),
    );
    on<DronePilotChanged>(
      (event, emit) => emit(state.copyWith(pilot: event.pilot)),
    );
    on<DroneTicked>((event, emit) => emit(state.copyWith(nowUs: event.nowUs)));
    on<DroneModeSelected>((event, _) => _pilot.selectMode(event.mode));
    on<DroneActionRequested>((event, _) => _pilot.perform(event.kind));
    on<DroneBackgrounded>((_, _) => _pilot.killNow());
  }

  final DroneManager _drones;
  final PilotManager _pilot;
  final TransportManager _transport;
  final List<StreamSubscription<Object?>> _subscriptions = [];

  Future<void> _onStarted(DroneStarted event, Emitter<DroneState> emit) async {
    await _cancel();
    _subscriptions.add(
      _drones.connection
          .where((connection) => connection.nodeId == state.nodeId)
          .listen((connection) => add(DroneConnectionChanged(connection))),
    );
    _subscriptions.add(
      _drones.status.listen((status) => add(DroneStatusChanged(status))),
    );
    _subscriptions.add(
      _pilot.state.listen((pilot) => add(DronePilotChanged(pilot))),
    );
    // The snapshot stream ticks at least every snapshot period: that is
    // the clock the ages on screen follow.
    _subscriptions.add(
      _transport.snapshots
          .map((snapshot) => snapshot.nowUs)
          .listen((nowUs) => add(DroneTicked(nowUs))),
    );
    await _drones.connect(state.nodeId);
    await _pilot.engage();
  }

  Future<void> _cancel() async {
    for (final subscription in _subscriptions) {
      await subscription.cancel();
    }
    _subscriptions.clear();
  }

  @override
  Future<void> close() async {
    await _cancel();
    await _pilot.disengage();
    await _drones.disconnect();
    return super.close();
  }
}
