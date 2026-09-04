import 'dart:async';

import 'package:logging/logging.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/manager.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/back/transport/node_kind.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:rxdart/rxdart.dart';

final Logger _log = Logger('back/drone');

/// The drones of the network and the one the user connected to. Reads the
/// transport's node table; the services that fly a drone (commands, flight
/// modes, telemetry) will hang off this manager and address [connection].
class DroneManager extends AbsManager {
  DroneManager(this._transport);

  final TransportManager _transport;
  final BehaviorSubject<DroneRoster> _roster = BehaviorSubject.seeded(
    DroneRoster.empty,
  );
  final BehaviorSubject<DroneConnection> _connection = BehaviorSubject.seeded(
    DroneConnection.none,
  );
  StreamSubscription<TransportSnapshot>? _subscription;
  int _targetId = broadcastNode;

  /// Every drone heard, from the nodes that announced a drone kind.
  ValueStream<DroneRoster> get roster => _roster.stream;

  /// The connected drone, [DroneConnection.none] when there is none.
  ValueStream<DroneConnection> get connection => _connection.stream;

  @override
  Future<bool> init() async {
    _subscription = _transport.snapshots.listen(_onSnapshot);
    return true;
  }

  @override
  Future<void> dispose() async {
    await _subscription?.cancel();
    _subscription = null;
    await _connection.close();
    await _roster.close();
  }

  /// Connects to the drone [nodeId]: follows it from now on, connected while
  /// it is heard, lost while it is not.
  Future<void> connect(int nodeId) async {
    _log.info('connect to ${formatNodeId(nodeId)}');
    _targetId = nodeId;
    _emit(DroneConnection(status: DroneLinkStatus.lost, nodeId: nodeId));
    _onSnapshot(_transport.snapshots.value);
  }

  /// Forgets the connected drone.
  Future<void> disconnect() async {
    if (_targetId == broadcastNode) {
      return;
    }
    _log.info('disconnect from ${formatNodeId(_targetId)}');
    _targetId = broadcastNode;
    _emit(DroneConnection.none);
  }

  void _onSnapshot(TransportSnapshot snapshot) {
    final drones = <DroneSummary>[];
    var others = 0;
    for (final node in snapshot.nodes) {
      final announce = node.announce;
      if (announce != null && isDroneKind(announce.kind)) {
        drones.add(
          DroneSummary(
            nodeId: node.id,
            name: announce.name,
            kind: announce.kind,
          ),
        );
      } else {
        ++others;
      }
    }
    drones.sort((a, b) => a.nodeId.compareTo(b.nodeId));
    final roster = DroneRoster(drones: drones, otherNodeCount: others);
    if (roster != _roster.value) {
      _roster.add(roster);
    }
    if (_targetId == broadcastNode) {
      return;
    }
    final current = _connection.value;
    final node = snapshot.node(_targetId);
    final announce = node?.announce;
    if (node == null || announce == null) {
      // Silent, or heard but not announced yet: what was known stays.
      _emit(
        DroneConnection(
          status: DroneLinkStatus.lost,
          nodeId: _targetId,
          info: current.info,
        ),
      );
      return;
    }
    _emit(
      DroneConnection(
        status: DroneLinkStatus.connected,
        nodeId: _targetId,
        info: DroneInfo.fromNode(node, announce, snapshot.nowUs),
      ),
    );
  }

  void _emit(DroneConnection connection) {
    if (connection.status != _connection.value.status) {
      _log.info(
        'drone ${formatNodeId(connection.nodeId)}: ${connection.status.name}',
      );
    }
    if (connection != _connection.value) {
      _connection.add(connection);
    }
  }
}
