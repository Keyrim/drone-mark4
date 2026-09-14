import 'dart:async';

import 'package:logging/logging.dart';
import 'package:mark4/back/discovery/directory_models.dart';
import 'package:mark4/back/discovery/discovery_directory.dart';
import 'package:mark4/back/manager.dart';
import 'package:mark4/back/messaging/messenger.dart';
import 'package:mark4/back/platform/abs_platform.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/back/transport/frame.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/back/transport/udp_link.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:mark4/gen/wire_hash.dart';
import 'package:rxdart/rxdart.dart';

final Logger _log = Logger('back/transport');

/// Opens one transport node; null when it cannot.
typedef TransportNodeFactory =
    Future<AbsTransportNode?> Function(int nodeId, int discoveryPort);

/// The phone as a node of the system: one UDP link on the shared discovery
/// port, the node table below and the dispatch above, polled on a timer.
///
/// It owns the whole communication stack of the phone and boots it in that
/// order: the node, the messenger over it, the discovery directory over the
/// messenger. Every manager above reads the node table as [snapshots], who
/// is around as [directory], and speaks the wire through [send] or a handler
/// registered on the [messenger].
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

  /// An Announce name holds this many characters.
  static const int maxAnnounceName = 16;

  final AbsPlatform _platform;
  final TransportNodeFactory _openNode;
  final int Function() _drawNodeId;
  final int Function() _clockUs;

  /// The one UDP port every node of a deployment agrees on.
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

  AbsTransportNode? _node;
  Messenger? _messenger;
  DiscoveryDirectory? _directory;
  Timer? _timer;
  int _lastSnapshotUs = 0;

  /// What [directory] answers before init() built the real one.
  final BehaviorSubject<DirectorySnapshot> _noDirectory =
      BehaviorSubject.seeded(DirectorySnapshot.empty);
  DirectorySnapshot _lastDirectory = DirectorySnapshot.empty;

  /// This node's id and Announce name.
  ValueStream<TransportIdentity> get identity => _identity.stream;

  /// The node table, refreshed when it changes and every [snapshotPeriod].
  ValueStream<TransportSnapshot> get snapshots => _snapshots.stream;

  /// Who is around: one entry per node heard, its identity once it answered.
  /// Empty until [init] built the directory.
  ValueStream<DirectorySnapshot> get directory =>
      _directory?.snapshot ?? _noDirectory.stream;

  /// The dispatch: where a manager registers what it consumes. Null until
  /// [init] built it, which the backend runs before every manager above
  /// this one, and again after [dispose].
  Messenger? get messenger => _messenger;

  /// Payloads that were not an Envelope; a bench running another schema
  /// shows here.
  int get decodeErrors => _messenger?.undecodable ?? 0;

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
    final node = await _openNode(nodeId, discoveryPort);
    if (node == null) {
      _log.severe('transport initialization failed on udp/$discoveryPort');
      return false;
    }
    _node = node;
    final messenger = Messenger(node);
    _messenger = messenger;
    final name = announceName(await _platform.deviceName());
    final directory = DiscoveryDirectory(
      messenger: messenger,
      node: node,
      self: Announce()
        ..kind = NodeKind.PHONE
        ..name = name
        ..mcu = Mcu.MCU_UNSPECIFIED
        ..wireHash = wireHash,
      ownWireHash: wireHash,
    );
    if (!messenger.register(directory)) {
      _log.severe('the dispatch table refused the discovery directory');
      return false;
    }
    _directory = directory;
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
    await _directory?.dispose();
    _directory = null;
    _messenger = null;
    _node?.dispose();
    _node = null;
    await _platform.releaseMulticastLock();
    await _noDirectory.close();
    await _snapshots.close();
    await _identity.close();
  }

  /// Sends one Envelope to [dst]. False when the frame left on no link (an
  /// unknown node, a payload too long, a broadcast destination).
  Future<bool> send(int dst, Envelope envelope) async =>
      _messenger?.send(dst, envelope) ?? false;

  /// One poll of the whole stack: the node drains its link and the messenger
  /// dispatches what came for this node, then the directory asks what it has
  /// to ask, then the node table goes out as a snapshot. What the timer does
  /// every [pollPeriod].
  void pollNow() {
    final node = _node;
    final messenger = _messenger;
    final directory = _directory;
    if (node == null || messenger == null || directory == null) {
      return;
    }
    final nowUs = _clockUs();
    messenger.poll(nowUs);
    directory.tick(nowUs);
    final snapshot = TransportSnapshot(
      nowUs: nowUs,
      nodes: node.nodes(),
      stats: node.stats(),
    );
    final identities = directory.snapshot.value;
    final changed =
        !snapshot.sameTable(_snapshots.value) || identities != _lastDirectory;
    if (changed || nowUs - _lastSnapshotUs >= snapshotPeriod.inMicroseconds) {
      _lastSnapshotUs = nowUs;
      _lastDirectory = identities;
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

  static int Function() _stopwatchClock() {
    final stopwatch = Stopwatch()..start();
    return () => stopwatch.elapsedMicroseconds;
  }
}
