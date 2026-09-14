import 'package:logging/logging.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/back/transport/frame.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:protobuf/protobuf.dart';

final Logger _log = Logger('back/messaging');

/// Consumer of the messages of some Envelope body cases. It names the cases
/// it claims and is registered on the messenger by whoever owns it; the same
/// case is never claimed twice.
abstract class AbsMessageHandler {
  /// The Envelope body cases this handler consumes.
  List<Envelope_Body> get bodyCases;

  /// One decoded message of one of those cases.
  /// [src] is the node it came from, where an answer goes; [nowUs] the
  /// instant of the poll that delivered it. Returns true when the handler
  /// acted on the message, false when it ignored it.
  bool onMessage(int src, Envelope envelope, int nowUs);
}

/// The postman and the sender: the one place an Envelope meets the
/// transport, in both directions. Inbound, every payload the node delivers
/// is decoded once and handed to the one handler that claimed its body case;
/// outbound, [send] encodes one message and unicasts it. No queue, no retry,
/// no broadcast.
class Messenger {
  Messenger(this._node);

  final AbsTransportNode _node;
  final Map<Envelope_Body, AbsMessageHandler> _handlers = {};
  int _received = 0;
  int _undecodable = 0;
  int _unhandled = 0;
  int _handled = 0;
  int _ignored = 0;
  int _sent = 0;
  int _refused = 0;

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

  /// [send] calls whose frame left on the link.
  int get sent => _sent;

  /// [send] calls refused: a broadcast destination, or a transport that took
  /// nothing.
  int get refused => _refused;

  /// Claims every body case of [handler]. False when one of them is already
  /// claimed: the handler hears nothing on that case, which is a composition
  /// error and is logged as one.
  bool register(AbsMessageHandler handler) {
    var ok = true;
    for (final bodyCase in handler.bodyCases) {
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

  /// Releases every case [handler] holds.
  void unregister(AbsMessageHandler handler) =>
      _handlers.removeWhere((_, owner) => identical(owner, handler));

  /// Polls the node once; every delivered payload is decoded and its handler
  /// is called. The one caller of the node's poll().
  void poll(int nowUs) {
    _node.poll(nowUs);
    for (
      var payload = _node.nextPayload();
      payload != null;
      payload = _node.nextPayload()
    ) {
      _onPayload(payload, nowUs);
    }
  }

  /// Encodes one message and unicasts it. [broadcastNode] is refused: what
  /// every node must hear is not a message to one node.
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

  void _onPayload(InboundPayload payload, int nowUs) {
    ++_received;
    final Envelope envelope;
    try {
      envelope = Envelope.fromBuffer(payload.bytes);
    } on InvalidProtocolBufferException {
      ++_undecodable;
      return;
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
