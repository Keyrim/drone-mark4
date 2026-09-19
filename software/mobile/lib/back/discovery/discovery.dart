import 'package:mark4/back/messaging/messenger.dart';
import 'package:mark4/gen/mark4.pb.dart';

/// Answers "who are you": this node's identity, unicast to whoever asks.
/// Every node carries one; a node that cannot say who it is does not exist
/// for the ground tools. Presence is the transport's keepalive and carries
/// no identity, so nothing is broadcast and nothing is unsolicited.
class Discovery extends AbsMessageHandler {
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
    // An answer that matters is a request of its own: the messenger keeps it
    // until the requester acknowledges it, and gives up on its own policy.
    // Whether this one was taken is that table's business.
    messenger.request(src, Envelope()..announce = self, this);
    ++_answered;
    return true;
  }
}
