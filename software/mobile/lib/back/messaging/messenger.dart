import 'dart:async';
import 'dart:typed_data';

import 'package:logging/logging.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/back/transport/frame.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:protobuf/protobuf.dart';

final Logger _log = Logger('back/messaging');

/// How long one request is retried, and how often. Per call, because a
/// reboot and a table page do not want the same values.
class RequestPolicy {
  const RequestPolicy({this.periodUs = 500000, this.retries = 5});

  /// Silence after a send before the next one [us].
  final int periodUs;

  /// Sends before giving up, the first one included.
  final int retries;
}

/// Consumer of the messages of some Envelope body cases. It names the cases
/// it claims and is registered on the messenger by whoever owns it; the same
/// case is never claimed twice. What a node's arrival and departure mean
/// reaches it through the messenger, which is the one presence listener of
/// this side.
abstract class AbsMessageHandler {
  /// The Envelope body cases this handler consumes.
  List<Envelope_Body> get bodyCases;

  /// One decoded message of one of those cases.
  /// [src] is the node it came from, where an answer goes; [nowUs] the
  /// instant of the poll that delivered it. Returns true when the handler
  /// acted on the message, false when it ignored it.
  bool onMessage(int src, Envelope envelope, int nowUs);

  /// One of this handler's requests was never acknowledged and is given up
  /// on. Does nothing by default.
  void onRequestFailed(int dst, int requestId) {}

  /// A node appeared on the transport. Does nothing by default.
  void onNodeUp(int nodeId) {}

  /// A node went down: everything this handler knew of it is stale. Does
  /// nothing by default.
  void onNodeDown(int nodeId) {}
}

/// One request waiting for its acknowledgement, as the messenger keeps it:
/// the encoded bytes and what the retry needs.
class _Pending {
  _Pending({
    required this.dst,
    required this.id,
    required this.bytes,
    required this.owner,
    required this.policy,
    required this.sentUs,
  });

  /// Node the request went to.
  final int dst;

  /// Request id, never 0.
  final int id;

  /// What is resent, as encoded.
  final Uint8List bytes;

  /// Told when the request is given up on.
  final AbsMessageHandler owner;

  /// How long this one is retried.
  final RequestPolicy policy;

  /// Instant of the last send [us].
  int sentUs;

  /// Sends so far, the first one included.
  int sends = 1;
}

/// The postman and the sender: the one place an Envelope meets the
/// transport, in both directions. Inbound, every payload the node delivers
/// is decoded once, acknowledged when it carries a request id, and handed to
/// the one handler that claimed its body case; outbound, [send] encodes one
/// message and unicasts it once, and [request] numbers it, keeps it and
/// resends it until the destination acknowledges it. No broadcast.
class Messenger {
  Messenger(this._node) {
    _presence = _node.presence.listen(_onPresence);
  }

  /// Requests waiting for their acknowledgement at once: the phone is a
  /// ground node, where one node appearing costs several at once.
  static const int maxPendingRequests = 32;

  final AbsTransportNode _node;
  final Map<Envelope_Body, AbsMessageHandler> _handlers = {};
  final Map<(int dst, int id), _Pending> _pending = {};
  StreamSubscription<PresenceEvent>? _presence;
  int _nowUs = 0;
  int _lastId = 0;
  int _received = 0;
  int _undecodable = 0;
  int _unhandled = 0;
  int _handled = 0;
  int _ignored = 0;
  int _sent = 0;
  int _refused = 0;
  int _requests = 0;
  int _resent = 0;
  int _completed = 0;
  int _failed = 0;
  int _acked = 0;
  int _unmatchedAcks = 0;

  /// Payloads the transport delivered.
  int get received => _received;

  /// Payloads that were not a valid Envelope.
  int get undecodable => _undecodable;

  /// Decoded messages with no handler for their body case.
  int get unhandled => _unhandled;

  /// Messages whose handler acted on them.
  int get handled => _handled;

  /// Messages whose handler ignored them.
  int get ignored => _ignored;

  /// Frames that left on the link: one per [send], one per send of a
  /// request.
  int get sent => _sent;

  /// [send] and [request] calls refused: a broadcast destination, a
  /// destination the transport does not know ([request] only), a full
  /// pending table, or a transport that took nothing.
  int get refused => _refused;

  /// Requests started.
  int get requests => _requests;

  /// Requests sent again because no acknowledgement came.
  int get resent => _resent;

  /// Requests acknowledged by their destination.
  int get completed => _completed;

  /// Requests given up on, out of sends or with the node that went down.
  int get failed => _failed;

  /// Acknowledgements this node sent, one per numbered message it received.
  int get acked => _acked;

  /// Acknowledgements that matched no pending request: a request already
  /// given up on, or one answered twice.
  int get unmatchedAcks => _unmatchedAcks;

  /// Claims every body case of [handler]. False when one of them is already
  /// claimed, or is the acknowledgement, which is the messenger's own: the
  /// handler hears nothing on that case, which is a composition error and is
  /// logged as one.
  bool register(AbsMessageHandler handler) {
    var ok = true;
    for (final bodyCase in handler.bodyCases) {
      if (bodyCase == Envelope_Body.ack) {
        _log.severe('${bodyCase.name} belongs to the messenger');
        ok = false;
        continue;
      }
      final owner = _handlers[bodyCase];
      if (owner != null) {
        _log.severe(
          '${bodyCase.name} is already handled by ${owner.runtimeType}',
        );
        ok = false;
        continue;
      }
      _handlers[bodyCase] = handler;
    }
    return ok;
  }

  /// Releases every case [handler] holds, and forgets the requests it owns:
  /// a handler that goes away leaves nobody to tell about them.
  void unregister(AbsMessageHandler handler) {
    _handlers.removeWhere((_, owner) => identical(owner, handler));
    _pending.removeWhere((_, entry) => identical(entry.owner, handler));
  }

  /// Polls the node once; every delivered payload is decoded, acknowledged
  /// when it carries a request id, and its handler is called. [tick] runs at
  /// the end, so a composition adds no call of its own. The one caller of
  /// the node's poll().
  void poll(int nowUs) {
    _nowUs = nowUs;
    _node.poll(nowUs);
    for (
      var payload = _node.nextPayload();
      payload != null;
      payload = _node.nextPayload()
    ) {
      _onPayload(payload, nowUs);
    }
    tick(nowUs);
  }

  /// Resends what went unanswered and gives up on what is past its policy.
  /// Called by [poll].
  void tick(int nowUs) {
    for (final entry in _pending.values.toList(growable: false)) {
      if (nowUs - entry.sentUs < entry.policy.periodUs) {
        continue;
      }
      if (entry.sends < entry.policy.retries) {
        ++entry.sends;
        entry.sentUs = nowUs;
        ++_resent;
        _emit(entry);
        continue;
      }
      // Out of sends: the destination is there for the transport and deaf
      // to this message. Its owner decides what that means.
      _pending.remove((entry.dst, entry.id));
      ++_failed;
      entry.owner.onRequestFailed(entry.dst, entry.id);
    }
  }

  /// Encodes one message and unicasts it, once: the streams, which are only
  /// worth their own instant. [broadcastNode] is refused: what every node
  /// must hear is not a message to one node.
  bool send(int dst, Envelope envelope) {
    if (dst == broadcastNode) {
      ++_refused;
      return false;
    }
    if (!_node.send(dst, envelope.writeToBuffer())) {
      ++_refused;
      return false;
    }
    ++_sent;
    return true;
  }

  /// Numbers one message, unicasts it and keeps it until [dst] acknowledges
  /// it, then gives up on it with [AbsMessageHandler.onRequestFailed] of
  /// [owner]. The request id is written into [envelope] and returned; 0 says
  /// the request was refused: a broadcast destination, a node the transport
  /// does not know (the caller acts on [AbsMessageHandler.onNodeUp]), or a
  /// full pending table. A first send the transport refuses is kept all the
  /// same, because the retry is exactly what covers it.
  int request(
    int dst,
    Envelope envelope,
    AbsMessageHandler owner, {
    RequestPolicy policy = const RequestPolicy(),
  }) {
    if (dst == broadcastNode ||
        _node.findNode(dst) == null ||
        _pending.length >= maxPendingRequests) {
      ++_refused;
      return 0;
    }
    final id = _nextId();
    envelope.requestId = id;
    final entry = _Pending(
      dst: dst,
      id: id,
      bytes: envelope.writeToBuffer(),
      owner: owner,
      policy: policy,
      // The instant of the poll in progress, or of the last one when the
      // request is started outside a poll: the retry is paced in periods of
      // half a second, which that offset never crosses.
      sentUs: _nowUs,
    );
    _pending[(dst, id)] = entry;
    ++_requests;
    _emit(entry);
    return id;
  }

  /// Stops following the transport's presence.
  Future<void> dispose() async {
    await _presence?.cancel();
    _presence = null;
    _pending.clear();
  }

  /// The next request id of this node: a counter of its own, never 0, so a
  /// request is the pair (node, id).
  int _nextId() {
    _lastId = (_lastId + 1) & 0xFFFFFFFF;
    if (_lastId == 0) {
      _lastId = 1;
    }
    return _lastId;
  }

  /// Hands one encoded pending request to the transport.
  void _emit(_Pending entry) {
    if (_node.send(entry.dst, entry.bytes)) {
      ++_sent;
      return;
    }
    ++_refused;
  }

  /// Sends one acknowledgement back to the node that asked. A raw send of
  /// its own: an acknowledgement is not one of this node's messages and does
  /// not count in [sent].
  void _acknowledge(int dst, int requestId) {
    final ack = Envelope()
      ..ack = RequestAck()
      ..requestId = requestId;
    if (_node.send(dst, ack.writeToBuffer())) {
      ++_acked;
    }
  }

  /// Completes the pending request one acknowledgement answers.
  void _onAck(int src, int requestId) {
    if (_pending.remove((src, requestId)) == null) {
      ++_unmatchedAcks;
      return;
    }
    ++_completed;
  }

  void _onPresence(PresenceEvent event) {
    if (!event.up) {
      // Nothing addressed to a node that left can still arrive: its
      // requests are given up on at once, whatever their policy had left.
      for (final entry in _pending.values.toList(growable: false)) {
        if (entry.dst != event.node.id) {
          continue;
        }
        _pending.remove((entry.dst, entry.id));
        ++_failed;
        entry.owner.onRequestFailed(entry.dst, entry.id);
      }
    }
    // One call per handler, not per case: a handler claiming several cases
    // sits in several slots.
    for (final handler in {..._handlers.values}) {
      if (event.up) {
        handler.onNodeUp(event.node.id);
      } else {
        handler.onNodeDown(event.node.id);
      }
    }
  }

  void _onPayload(InboundPayload payload, int nowUs) {
    ++_received;
    final Envelope envelope;
    try {
      envelope = Envelope.fromBuffer(payload.bytes);
    } on InvalidProtocolBufferException {
      ++_undecodable;
      return;
    }
    if (envelope.whichBody() == Envelope_Body.ack) {
      // The acknowledgement case is the messenger's own: register() refuses
      // a handler that claims it, and it is never dispatched.
      _onAck(payload.src, envelope.requestId);
      return;
    }
    if (envelope.requestId != 0) {
      // Acknowledged before dispatch, whether or not a handler claims the
      // case: it says the message arrived, not what was done with it.
      _acknowledge(payload.src, envelope.requestId);
    }
    final handler = _handlers[envelope.whichBody()];
    if (handler == null) {
      ++_unhandled;
      return;
    }
    if (handler.onMessage(payload.src, envelope, nowUs)) {
      ++_handled;
    } else {
      ++_ignored;
    }
  }
}
