import 'package:equatable/equatable.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';

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

/// The transport as of one poll: every live node and the counters. Who the
/// nodes are is the directory's business, not this one's.
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
  final List<NodeInfo> nodes;
  final TransportStats stats;

  /// The node with this id, null when not live.
  NodeInfo? node(int nodeId) {
    for (final node in nodes) {
      if (node.id == nodeId) {
        return node;
      }
    }
    return null;
  }

  /// True when the two snapshots list the same nodes: what the counters and
  /// the instants change every poll, this does not.
  bool sameTable(TransportSnapshot other) {
    if (nodes.length != other.nodes.length) {
      return false;
    }
    for (var index = 0; index < nodes.length; ++index) {
      if (nodes[index].id != other.nodes[index].id) {
        return false;
      }
    }
    return true;
  }

  @override
  List<Object?> get props => [nowUs, nodes, stats];
}
