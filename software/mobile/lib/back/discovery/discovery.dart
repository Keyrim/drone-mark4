import 'package:mark4/back/messaging/messenger.dart';
import 'package:mark4/gen/mark4.pb.dart';

/// Answers "who are you": this node's identity, unicast to whoever asks.
/// Every node carries one; a node that cannot say who it is does not exist
/// for the ground tools. Presence is the transport's keepalive and carries
/// no identity, so nothing is broadcast and nothing is unsolicited.
class Discovery implements AbsMessageHandler {
  Discovery(this.messenger, this.self);

  /// The messenger the requests come from and the answers leave by.
  final Messenger messenger;

  /// This node's identity, as it is answered.
  final Announce self;

  int _answered = 0;

  /// Requests answered since construction.
  int get answered => _answered;

  @override
  List<Envelope_Body> get bodyCases => const [Envelope_Body.identityRequest];

  @override
  bool onMessage(int src, Envelope envelope, int nowUs) {
    // Whether the frame left is the messenger's count: the requester asks
    // again if it did not.
    messenger.send(src, Envelope()..announce = self);
    ++_answered;
    return true;
  }
}
