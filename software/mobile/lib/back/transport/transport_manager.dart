import 'dart:async';

import 'package:logging/logging.dart';
import 'package:mark4/back/manager.dart';
import 'package:mark4/back/platform/abs_platform.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:mark4/gen/wire_hash.dart';
import 'package:protobuf/protobuf.dart';
import 'package:rxdart/rxdart.dart';

final Logger _log = Logger('back/transport');

/// Opens one transport node; null when it cannot.
typedef TransportNodeFactory =
    AbsTransportNode? Function(int nodeId, int discoveryPort);

/// The phone as a node of the system: one UDP link on the shared discovery
/// port, the Announce beacon, and a poll of the C++ transport on a timer.
/// Exposes the node table as [snapshots] and every Envelope addressed to
/// this node as [envelopes]; the services of the other managers speak the
/// wire through [send].
class TransportManager extends AbsManager {
  TransportManager({
    required this._platform,
    required this._openNode,
    required this._drawNodeId,
    int Function()? clockUs,
    this.discoveryPort = defaultDiscoveryPort,
    this.pollPeriod = const Duration(milliseconds: 10),
    this.snapshotPeriod = const Duration(milliseconds: 500),
  }) : _clockUs = clockUs ?? _stopwatchClock();

  /// The one UDP port every node of a deployment agrees on.
  static const int defaultDiscoveryPort = 47820;

  /// An Announce name holds this many characters.
  static const int maxAnnounceName = 16;

  final AbsPlatform _platform;
  final TransportNodeFactory _openNode;
  final int Function() _drawNodeId;
  final int Function() _clockUs;
  final int discoveryPort;

  /// Cadence of the transport poll; null when the owner calls [pollNow]
  /// itself (tests).
  final Duration? pollPeriod;

  /// Cadence of a snapshot when only counters changed.
  final Duration snapshotPeriod;

  final BehaviorSubject<TransportIdentity> _identity = BehaviorSubject.seeded(
    TransportIdentity.none,
  );
  final BehaviorSubject<TransportSnapshot> _snapshots = BehaviorSubject.seeded(
    TransportSnapshot.empty,
  );
  final PublishSubject<InboundEnvelope> _envelopes = PublishSubject();
  final Map<int, NodeAnnounce> _announces = {};

  AbsTransportNode? _node;
  Timer? _timer;
  int _lastSnapshotUs = 0;
  int _decodeErrors = 0;

  /// This node's id and Announce name.
  ValueStream<TransportIdentity> get identity => _identity.stream;

  /// The node table, refreshed when it changes and every [snapshotPeriod].
  ValueStream<TransportSnapshot> get snapshots => _snapshots.stream;

  /// Every Envelope addressed to this node, Announces included.
  Stream<InboundEnvelope> get envelopes => _envelopes.stream;

  /// Envelopes that were not one; a bench running another schema shows here.
  int get decodeErrors => _decodeErrors;

  /// The transport's clock now [us]: the base of every instant in the
  /// snapshots, for whoever stamps something against them.
  int nowUs() => _clockUs();

  @override
  Future<bool> init() async {
    if (!await _platform.acquireMulticastLock()) {
      // Not fatal: unicasts still flow, and a phone on a hotspot of its own
      // may not need the lock. The node table tells.
      _log.warning('no multicast lock: broadcasts may be dropped');
    }
    final nodeId = _drawNodeId();
    if (nodeId == broadcastNode) {
      _log.severe('no node id: the random source could not be read');
      return false;
    }
    final node = _openNode(nodeId, discoveryPort);
    if (node == null) {
      _log.severe('transport initialization failed on udp/$discoveryPort');
      return false;
    }
    _node = node;
    final name = announceName(await _platform.deviceName());
    final beacon = Envelope()
      ..announce = (Announce()
        ..kind = NodeKind.PHONE
        ..name = name
        ..mcu = Mcu.MCU_UNSPECIFIED
        ..wireHash = wireHash);
    if (!node.setBeacon(beacon.writeToBuffer())) {
      _log.severe('the announce does not fit a beacon');
      return false;
    }
    _identity.add(TransportIdentity(nodeId: nodeId, name: name));
    final period = pollPeriod;
    if (period != null) {
      _timer = Timer.periodic(period, (_) => pollNow());
    }
    _log.info(
      'boot: node ${formatNodeId(nodeId)} "$name" on discovery udp/$discoveryPort, '
      'wire ${formatNodeId(wireHash)}',
    );
    return true;
  }

  @override
  Future<void> dispose() async {
    _timer?.cancel();
    _timer = null;
    _node?.dispose();
    _node = null;
    await _platform.releaseMulticastLock();
    await _envelopes.close();
    await _snapshots.close();
    await _identity.close();
  }

  /// Sends one Envelope to [dst], [broadcastNode] for every node. False when
  /// the frame left on no link (unknown node, payload too long).
  Future<bool> send(int dst, Envelope envelope) async {
    final node = _node;
    if (node == null) {
      return false;
    }
    return node.send(dst, envelope.writeToBuffer());
  }

  /// One poll of the transport: what the timer does every [pollPeriod].
  void pollNow() {
    final node = _node;
    if (node == null) {
      return;
    }
    final nowUs = _clockUs();
    node.poll(nowUs);
    for (
      var payload = node.nextPayload();
      payload != null;
      payload = node.nextPayload()
    ) {
      _deliver(payload);
    }
    final infos = node.nodes();
    final liveIds = {for (final info in infos) info.id};
    _announces.removeWhere((id, _) => !liveIds.contains(id));
    final snapshot = TransportSnapshot(
      nowUs: nowUs,
      nodes: [
        for (final info in infos)
          TransportNode(info: info, announce: _announces[info.id]),
      ],
      stats: node.stats(),
    );
    final tableChanged = !snapshot.sameTable(_snapshots.value);
    if (tableChanged ||
        nowUs - _lastSnapshotUs >= snapshotPeriod.inMicroseconds) {
      _lastSnapshotUs = nowUs;
      _snapshots.add(snapshot);
    }
  }

  /// Cuts a device name to what an Announce name holds, ASCII only.
  static String announceName(String deviceName) {
    final ascii = deviceName.replaceAll(RegExp(r'[^\x20-\x7E]'), '').trim();
    final name = ascii.isEmpty ? 'phone' : ascii;
    return name.length <= maxAnnounceName
        ? name
        : name.substring(0, maxAnnounceName);
  }

  void _deliver(InboundPayload payload) {
    final Envelope envelope;
    try {
      envelope = Envelope.fromBuffer(payload.bytes);
    } on InvalidProtocolBufferException {
      ++_decodeErrors;
      return;
    }
    if (envelope.hasAnnounce()) {
      _announces[payload.src] = NodeAnnounce.fromWire(
        envelope.announce,
        wireHash,
      );
    }
    _envelopes.add(InboundEnvelope(src: payload.src, envelope: envelope));
  }

  static int Function() _stopwatchClock() {
    final stopwatch = Stopwatch()..start();
    return () => stopwatch.elapsedMicroseconds;
  }
}
