import 'package:equatable/equatable.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/gen/mark4.pb.dart';

/// What a node said about itself in its last Announce.
class NodeAnnounce extends Equatable {
  const NodeAnnounce({
    required this.kind,
    required this.name,
    required this.mcu,
    required this.gitHash,
    required this.buildEpoch,
    required this.wireHash,
    required this.wireMismatch,
  });

  /// From the wire; [ownWireHash] is what this build speaks.
  factory NodeAnnounce.fromWire(Announce announce, int ownWireHash) =>
      NodeAnnounce(
        kind: announce.kind,
        name: announce.name,
        mcu: announce.mcu,
        gitHash: announce.gitHash,
        buildEpoch: announce.buildEpoch,
        wireHash: announce.wireHash,
        wireMismatch: announce.wireHash != ownWireHash,
      );

  final NodeKind kind;

  /// Free label, at most 16 characters.
  final String name;
  final Mcu mcu;

  /// Short commit hash, empty when unknown.
  final String gitHash;

  /// Packaging time [unix s], 0 when unknown.
  final int buildEpoch;

  /// Hash of mark4.proto as built into the node.
  final int wireHash;

  /// The node was built on another schema.
  final bool wireMismatch;

  @override
  List<Object?> get props => [
    kind,
    name,
    mcu,
    gitHash,
    buildEpoch,
    wireHash,
    wireMismatch,
  ];
}

/// One node heard on the link: the transport's view, plus its Announce once
/// it beaconed.
class TransportNode extends Equatable {
  const TransportNode({required this.info, this.announce});

  final NodeInfo info;

  /// Null until the node beaconed.
  final NodeAnnounce? announce;

  int get id => info.id;

  @override
  List<Object?> get props => [info, announce];
}

/// This node, as it announces itself.
class TransportIdentity extends Equatable {
  const TransportIdentity({required this.nodeId, required this.name});

  static const TransportIdentity none = TransportIdentity(nodeId: 0, name: '');

  /// 0 before init().
  final int nodeId;

  /// The Announce name.
  final String name;

  @override
  List<Object?> get props => [nodeId, name];
}

/// The transport as of one poll: every live node and the counters.
class TransportSnapshot extends Equatable {
  const TransportSnapshot({
    required this.nowUs,
    required this.nodes,
    required this.stats,
  });

  static const TransportSnapshot empty = TransportSnapshot(
    nowUs: 0,
    nodes: [],
    stats: TransportStats(),
  );

  /// Instant of the poll, same clock as NodeInfo.lastSeenUs.
  final int nowUs;
  final List<TransportNode> nodes;
  final TransportStats stats;

  /// The node with this id, null when not live.
  TransportNode? node(int nodeId) {
    for (final node in nodes) {
      if (node.id == nodeId) {
        return node;
      }
    }
    return null;
  }

  /// True when the two snapshots list the same nodes with the same
  /// announces: what the counters and instants change every poll, this does
  /// not.
  bool sameTable(TransportSnapshot other) {
    if (nodes.length != other.nodes.length) {
      return false;
    }
    for (var index = 0; index < nodes.length; ++index) {
      if (nodes[index].id != other.nodes[index].id ||
          nodes[index].announce != other.nodes[index].announce) {
        return false;
      }
    }
    return true;
  }

  @override
  List<Object?> get props => [nowUs, nodes, stats];
}

/// One decoded Envelope addressed to this node.
class InboundEnvelope {
  const InboundEnvelope({required this.src, required this.envelope});

  final int src;
  final Envelope envelope;
}
