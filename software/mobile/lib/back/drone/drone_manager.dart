import 'dart:async';
import 'dart:math' as math;

import 'package:logging/logging.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/drone/status_consumer.dart';
import 'package:mark4/back/manager.dart';
import 'package:mark4/back/transport/frame.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/back/transport/node_kind.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:rxdart/rxdart.dart';

final Logger _log = Logger('back/drone');

/// The drones of the network, the one the user connected to, and what that
/// one reports. Reads the discovery directory and the transport's node
/// table, and the Status stream of the connected drone, which it subscribes
/// to as soon as the node is heard; the pilot service addresses
/// [connection].
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
  StatusConsumer? _statusConsumer;
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
    final messenger = _transport.messenger;
    if (messenger == null) {
      _log.severe('the transport holds no messenger');
      return false;
    }
    final consumer = StatusConsumer(messenger, _onStatus);
    if (!messenger.register(consumer)) {
      _log.severe('the dispatch table refused the Status consumer');
      return false;
    }
    _statusConsumer = consumer;
    _subscription = _transport.snapshots.listen(_onSnapshot);
    return true;
  }

  @override
  Future<void> dispose() async {
    final consumer = _statusConsumer;
    if (consumer != null) {
      _transport.messenger?.unregister(consumer);
    }
    _statusConsumer = null;
    await _subscription?.cancel();
    _subscription = null;
    await _status.close();
    await _connection.close();
    await _roster.close();
  }

  /// Connects to the drone [nodeId]: follows it from now on, connected while
  /// it is heard, lost while it is not, and subscribed to its Status stream
  /// for as long.
  Future<void> connect(int nodeId) async {
    _log.info('connect to ${formatNodeId(nodeId)}');
    _dropSubscription();
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
    _dropSubscription();
    _targetId = broadcastNode;
    if (_status.value != null) {
      _status.add(null);
    }
    _emit(DroneConnection.none);
  }

  /// One Status of the drone the user connected to; anything else is not
  /// this manager's business.
  bool _onStatus(int src, Status wire, int nowUs) {
    if (src != _targetId) {
      return false;
    }
    final status = DroneStatus.fromWire(wire, nowUs);
    final previous = _status.value;
    final phaseChanged = previous == null || previous.phase != status.phase;
    if (phaseChanged) {
      _log.info('drone ${formatNodeId(_targetId)}: phase ${status.phase.name}');
    }
    if (!phaseChanged && nowUs - _lastStatusUs < statusPeriod.inMicroseconds) {
      return true;
    }
    _lastStatusUs = nowUs;
    _status.add(status);
    return true;
  }

  void _onSnapshot(TransportSnapshot snapshot) {
    final directory = _transport.directory.value;
    final drones = [
      for (final entry in directory.nodesOfKind(droneKinds))
        DroneSummary(
          nodeId: entry.id,
          name: entry.announce!.name,
          kind: entry.announce!.kind,
        ),
    ];
    drones.sort((a, b) => a.nodeId.compareTo(b.nodeId));
    // Everything else on the network: another kind, or a node that has not
    // said who it is yet.
    final others = math.max(0, snapshot.nodes.length - drones.length);
    final roster = DroneRoster(drones: drones, otherNodeCount: others);
    if (roster != _roster.value) {
      _roster.add(roster);
    }
    if (_targetId == broadcastNode) {
      return;
    }
    final current = _connection.value;
    final node = snapshot.node(_targetId);
    final announce = directory.find(_targetId)?.announce;
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
    final consumer = _statusConsumer;
    if (current.status != DroneLinkStatus.connected &&
        consumer != null &&
        !consumer.subscribed) {
      // The drone streams its Status to the nodes that asked for it: the
      // subscription is taken the moment the transport holds the node, and
      // again after a reboot, which the transport reports as a node going
      // down and coming back.
      consumer.subscribe(_targetId);
    }
    _emit(
      DroneConnection(
        status: DroneLinkStatus.connected,
        nodeId: _targetId,
        info: DroneInfo.fromNode(node, announce, snapshot.nowUs),
      ),
    );
  }

  /// Tells the drone being let go to stop streaming, while the transport
  /// still knows where it is.
  void _dropSubscription() {
    final consumer = _statusConsumer;
    if (consumer == null || _targetId == broadcastNode) {
      return;
    }
    consumer.unsubscribe(_targetId);
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
