import 'package:logging/logging.dart';
import 'package:mark4/back/messaging/messenger.dart';
import 'package:mark4/back/transport/frame.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/gen/mark4.pb.dart';

final Logger _log = Logger('back/drone');

/// One Status report of a node, as the consumer hands it over.
typedef StatusSink = bool Function(int src, Status status, int nowUs);

/// The Status stream of the drone the phone follows: the subscribe out, the
/// reports in.
///
/// A drone emits its Status to the nodes that asked for it and to nobody
/// else, so the phone subscribes to the one it connected to and unsubscribes
/// when it lets it go. The subscribe is a request: the drone acknowledges
/// its arrival and answers with a StatusSubscription, the state it holds
/// afterwards, which is what [subscribed] holds. A request and its answer
/// are two message types, so a node that is provider and consumer of one
/// concept never claims one body case twice. A node that goes down was never told anything:
/// the subscription is dropped and the manager asks again when it comes
/// back.
class StatusConsumer extends AbsMessageHandler {
  StatusConsumer(this._messenger, this._onStatus);

  final Messenger _messenger;
  final StatusSink _onStatus;
  int _nodeId = broadcastNode;
  bool _subscribed = false;

  /// The drone whose stream is held or asked for, [broadcastNode] for none.
  int get nodeId => _nodeId;

  /// The node answered that it holds the phone in its subscriber table.
  bool get subscribed => _subscribed;

  @override
  List<Envelope_Body> get bodyCases => const [
    Envelope_Body.status,
    Envelope_Body.statusSubscription,
  ];

  /// Asks [nodeId] for its Status stream. Refused while the transport does
  /// not know the node: the caller asks again when it is heard.
  void subscribe(int nodeId) {
    _nodeId = nodeId;
    _subscribed = false;
    _send(nodeId, enabled: true);
  }

  /// Tells [nodeId] to stop streaming and forgets it.
  void unsubscribe(int nodeId) {
    _send(nodeId, enabled: false);
    if (_nodeId == nodeId) {
      _nodeId = broadcastNode;
      _subscribed = false;
    }
  }

  @override
  bool onMessage(int src, Envelope envelope, int nowUs) {
    if (envelope.whichBody() == Envelope_Body.status) {
      return _onStatus(src, envelope.status, nowUs);
    }
    if (src != _nodeId) {
      return false;
    }
    // The answer is the subscription the node holds afterwards, a message
    // type of its own: a full subscriber table answers false.
    _subscribed = envelope.statusSubscription.enabled;
    _log.info(
      'drone ${formatNodeId(src)}: status stream '
      '${_subscribed ? 'on' : 'off'}',
    );
    return true;
  }

  @override
  void onRequestFailed(int dst, int requestId) {
    if (dst != _nodeId) {
      return;
    }
    _log.warning(
      'drone ${formatNodeId(dst)}: no answer to the status subscription',
    );
    _subscribed = false;
  }

  @override
  void onNodeDown(int nodeId) {
    if (nodeId == _nodeId) {
      // It kept no subscriber table across its silence: whatever comes back
      // under that id is asked again.
      _subscribed = false;
    }
  }

  void _send(int nodeId, {required bool enabled}) {
    _messenger.request(
      nodeId,
      Envelope()..statusSubscribe = (StatusSubscribe()..enabled = enabled),
      this,
    );
  }
}
