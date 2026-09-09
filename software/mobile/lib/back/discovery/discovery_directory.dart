import 'dart:async';

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
  int requests = 0;

  DirectoryEntry get published =>
      DirectoryEntry(id: id, state: state, hops: hops, announce: announce);
}

/// A [Discovery] that also asks: who is around, by kind.
///
/// A node the transport hears gets a pending entry; the next [tick] asks it
/// who it is, and asks again every [identityTimeoutUs] up to
/// [identityRetries] times before leaving it mute. An Announce, at any time,
/// makes the entry known. A node the transport forgets takes its entry with
/// it. It never reads a clock: the instants come from the messenger's poll
/// and from [tick].
class DiscoveryDirectory extends Discovery {
  DiscoveryDirectory({
    required Messenger messenger,
    required AbsTransportNode node,
    required Announce self,
    required this.ownWireHash,
  }) : _node = node,
       super(messenger, self) {
    _presence = node.presence.listen(_onPresence);
  }

  /// Silence after a request before it is sent again [us].
  static const int identityTimeoutUs = 500000;

  /// Requests sent to a node before giving up on it.
  static const int identityRetries = 5;

  /// Entries kept at once: one per node the transport can hold.
  static const int maxEntries = 32;

  /// The schema this build speaks, against which an announce is compared.
  final int ownWireHash;

  final AbsTransportNode _node;
  final Map<int, _Entry> _entries = {};
  final BehaviorSubject<DirectorySnapshot> _snapshot = BehaviorSubject.seeded(
    DirectorySnapshot.empty,
  );
  StreamSubscription<PresenceEvent>? _presence;
  int _requests = 0;
  int _learnt = 0;
  int _muted = 0;
  int _dropped = 0;

  /// Who is around, as of the last change.
  ValueStream<DirectorySnapshot> get snapshot => _snapshot.stream;

  /// IdentityRequests sent, the refused ones included.
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
      entry = _Entry(id: src, hops: _node.findNode(src)?.hops ?? 0);
      _entries[src] = entry;
    }
    // A mute node that answers late is known like any other.
    entry
      ..state = DirectoryState.known
      ..announce = NodeAnnounce.fromWire(envelope.announce, ownWireHash)
      ..hops = _node.findNode(src)?.hops ?? entry.hops;
    ++_learnt;
    _publish();
    return true;
  }

  /// Asks, retries and gives up on time. Called from the loop, after the
  /// messenger's poll.
  void tick(int nowUs) {
    for (final entry in _entries.values) {
      if (entry.state != DirectoryState.pending) {
        continue;
      }
      if (entry.requests == 0) {
        _ask(entry, nowUs);
        continue;
      }
      if (nowUs - entry.askedUs < identityTimeoutUs) {
        continue;
      }
      if (entry.requests < identityRetries) {
        _ask(entry, nowUs);
        continue;
      }
      entry.state = DirectoryState.mute;
      ++_muted;
    }
    _publish();
  }

  /// Stops following the transport and closes the snapshot.
  Future<void> dispose() async {
    await _presence?.cancel();
    _presence = null;
    _entries.clear();
    await _snapshot.close();
  }

  void _onPresence(PresenceEvent event) {
    if (event.up) {
      _onNodeUp(event.node);
    } else {
      _onNodeDown(event.node);
    }
  }

  void _onNodeUp(NodeInfo node) {
    if (!_entries.containsKey(node.id) && _entries.length >= maxEntries) {
      ++_dropped;
      return;
    }
    // No instant here: the entry waits for the next tick(), which asks a
    // pending entry with no request behind it at once.
    _entries[node.id] = _Entry(id: node.id, hops: node.hops);
    _publish();
  }

  void _onNodeDown(NodeInfo node) {
    if (_entries.remove(node.id) != null) {
      _publish();
    }
  }

  /// Sends one IdentityRequest and counts it, whether or not the frame left:
  /// the transport may not hold the node's address yet on the very first
  /// tick, and the retry covers it.
  void _ask(_Entry entry, int nowUs) {
    messenger.send(entry.id, Envelope()..identityRequest = IdentityRequest());
    entry
      ..askedUs = nowUs
      ..requests = entry.requests + 1;
    ++_requests;
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
