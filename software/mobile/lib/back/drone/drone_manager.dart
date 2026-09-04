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

/// The drones of the network, the one the user connected to, and what that
/// one reports. Reads the transport's node table and the Status broadcasts
/// of the connected drone; the pilot service addresses [connection].
class DroneManager extends AbsManager {
  DroneManager(
    this._transport, {
    this.statusPeriod = const Duration(milliseconds: 50),
  });

  final TransportManager _transport;

  /// Least time between two [status] values with the same phase: Status
  /// lands at 50 Hz, a screen reads it slower; a phase change goes out at
  /// once.
  final Duration statusPeriod;

  final BehaviorSubject<DroneRoster> _roster = BehaviorSubject.seeded(
    DroneRoster.empty,
  );
  final BehaviorSubject<DroneConnection> _connection = BehaviorSubject.seeded(
    DroneConnection.none,
  );
  final BehaviorSubject<DroneStatus?> _status = BehaviorSubject.seeded(null);
  StreamSubscription<TransportSnapshot>? _subscription;
  StreamSubscription<InboundEnvelope>? _envelopes;
  int _targetId = broadcastNode;
  int _lastStatusUs = 0;

  /// Every drone heard, from the nodes that announced a drone kind.
  ValueStream<DroneRoster> get roster => _roster.stream;

  /// The connected drone, [DroneConnection.none] when there is none.
  ValueStream<DroneConnection> get connection => _connection.stream;

  /// The last Status of the connected drone, null until one arrived or
  /// after a disconnect. Kept while the drone is lost: it is what was last
  /// known.
  ValueStream<DroneStatus?> get status => _status.stream;

  @override
  Future<bool> init() async {
    _subscription = _transport.snapshots.listen(_onSnapshot);
    _envelopes = _transport.envelopes.listen(_onEnvelope);
    return true;
  }

  @override
  Future<void> dispose() async {
    await _envelopes?.cancel();
    _envelopes = null;
    await _subscription?.cancel();
    _subscription = null;
    await _status.close();
    await _connection.close();
    await _roster.close();
  }

  /// Connects to the drone [nodeId]: follows it from now on, connected while
  /// it is heard, lost while it is not.
  Future<void> connect(int nodeId) async {
    _log.info('connect to ${formatNodeId(nodeId)}');
    _targetId = nodeId;
    if (_status.value != null) {
      _status.add(null);
    }
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
    if (_status.value != null) {
      _status.add(null);
    }
    _emit(DroneConnection.none);
  }

  void _onEnvelope(InboundEnvelope inbound) {
    if (inbound.src != _targetId || !inbound.envelope.hasStatus()) {
      return;
    }
    final nowUs = _transport.nowUs();
    final status = DroneStatus.fromWire(inbound.envelope.status, nowUs);
    final previous = _status.value;
    final phaseChanged = previous == null || previous.phase != status.phase;
    if (phaseChanged) {
      _log.info('drone ${formatNodeId(_targetId)}: phase ${status.phase.name}');
    }
    if (!phaseChanged && nowUs - _lastStatusUs < statusPeriod.inMicroseconds) {
      return;
    }
    _lastStatusUs = nowUs;
    _status.add(status);
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
