import 'package:mark4/back/discovery/directory_models.dart';
import 'package:mark4/back/discovery/discovery.dart';
import 'package:mark4/back/messaging/messenger.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:rxdart/rxdart.dart';

/// One node the directory follows, with the bookkeeping of the asking that
/// no consumer of the snapshot cares about.
class _Entry {
  _Entry({required this.id, required this.hops});

  final int id;
  DirectoryState state = DirectoryState.pending;
  NodeAnnounce? announce;
  int hops;
  int askedUs = 0;

  /// Requests started, 0 or 1: the resends are the messenger's.
  int requests = 0;

  /// Id of the request in flight, 0 when none.
  int requestId = 0;

  DirectoryEntry get published =>
      DirectoryEntry(id: id, state: state, hops: hops, announce: announce);
}

/// A [Discovery] that also asks: who is around, by kind.
///
/// A node the transport hears gets a pending entry; the next [tick] asks it
/// who it is, as one request whose policy resends it every
/// [identityTimeoutUs] up to [identityRetries] sends before the messenger
/// gives up and the entry goes mute. An Announce, at any time, makes the
/// entry known. A node the transport forgets takes its entry with it. It
/// never reads a clock: the instants come from the messenger's poll and from
/// [tick], and presence reaches it through the messenger like every handler.
class DiscoveryDirectory extends Discovery {
  DiscoveryDirectory({
    required Messenger messenger,
    required this.node,
    required Announce self,
    required this.ownWireHash,
  }) : super(messenger, self);

  /// Silence after a request before it is sent again [us].
  static const int identityTimeoutUs = 500000;

  /// Requests sent to a node before giving up on it.
  static const int identityRetries = 5;

  /// Entries kept at once: one per node the transport can hold.
  static const int maxEntries = 32;

  /// The transport node, read for the distance in hops of an entry.
  final AbsTransportNode node;

  /// The schema this build speaks, against which an announce is compared.
  final int ownWireHash;

  final Map<int, _Entry> _entries = {};
  final BehaviorSubject<DirectorySnapshot> _snapshot = BehaviorSubject.seeded(
    DirectorySnapshot.empty,
  );
  int _requests = 0;
  int _learnt = 0;
  int _muted = 0;
  int _dropped = 0;

  /// Who is around, as of the last change.
  ValueStream<DirectorySnapshot> get snapshot => _snapshot.stream;

  /// IdentityRequests started, the refused ones included; what the messenger
  /// resent is its own counter.
  int get requests => _requests;

  /// Announces stored as an identity.
  int get learnt => _learnt;

  /// Nodes given up on after [identityRetries] requests.
  int get muted => _muted;

  /// Nodes that appeared while the table was full.
  int get dropped => _dropped;

  @override
  List<Envelope_Body> get bodyCases => const [
    Envelope_Body.identityRequest,
    Envelope_Body.announce,
  ];

  @override
  bool onMessage(int src, Envelope envelope, int nowUs) {
    if (envelope.whichBody() != Envelope_Body.announce) {
      return super.onMessage(src, envelope, nowUs);
    }
    var entry = _entries[src];
    if (entry == null) {
      // The transport learnt the node before this directory existed, or the
      // table was full when it appeared: the answer makes the entry, when
      // there is room for one.
      if (_entries.length >= maxEntries) {
        return true;
      }
      entry = _Entry(id: src, hops: node.findNode(src)?.hops ?? 0);
      _entries[src] = entry;
    }
    // A mute node that answers late is known like any other.
    entry
      ..state = DirectoryState.known
      ..announce = NodeAnnounce.fromWire(envelope.announce, ownWireHash)
      ..hops = node.findNode(src)?.hops ?? entry.hops;
    ++_learnt;
    _publish();
    return true;
  }

  /// Asks what nobody has asked yet. Called from the loop, after the
  /// messenger's poll; the retries and the giving up are the messenger's.
  void tick(int nowUs) {
    for (final entry in _entries.values) {
      // Only the entries nobody has asked yet: once a request is with the
      // messenger, the resends and the giving up are its business.
      if (entry.state == DirectoryState.pending && entry.requests == 0) {
        _ask(entry, nowUs);
      }
    }
    _publish();
  }

  @override
  void onNodeUp(int nodeId) {
    if (!_entries.containsKey(nodeId) && _entries.length >= maxEntries) {
      ++_dropped;
      return;
    }
    // No instant here: the entry waits for the next tick(), which asks a
    // pending entry with no request behind it at once.
    _entries[nodeId] = _Entry(
      id: nodeId,
      hops: node.findNode(nodeId)?.hops ?? 0,
    );
    _publish();
  }

  @override
  void onNodeDown(int nodeId) {
    if (_entries.remove(nodeId) != null) {
      _publish();
    }
  }

  @override
  void onRequestFailed(int dst, int requestId) {
    final entry = _entries[dst];
    if (entry == null ||
        entry.state != DirectoryState.pending ||
        entry.requestId != requestId) {
      // Answered in the meantime, or gone: the request that ran out of sends
      // has nothing left to say.
      return;
    }
    entry
      ..state = DirectoryState.mute
      ..requestId = 0;
    ++_muted;
    _publish();
  }

  /// Closes the snapshot.
  Future<void> dispose() async {
    _entries.clear();
    await _snapshot.close();
  }

  /// Sends one IdentityRequest and counts it, whether or not the messenger
  /// took it: the transport may not hold the node's address yet on the very
  /// first tick, and the next tick asks again.
  void _ask(_Entry entry, int nowUs) {
    final requestId = messenger.request(
      entry.id,
      Envelope()..identityRequest = IdentityRequest(),
      this,
      policy: const RequestPolicy(
        periodUs: identityTimeoutUs,
        retries: identityRetries,
      ),
    );
    ++_requests;
    if (requestId == 0) {
      return;
    }
    entry
      ..askedUs = nowUs
      ..requests = 1
      ..requestId = requestId;
  }

  void _publish() {
    final snapshot = DirectorySnapshot(
      entries: [for (final entry in _entries.values) entry.published],
    );
    if (snapshot != _snapshot.value) {
      _snapshot.add(snapshot);
    }
  }
}
