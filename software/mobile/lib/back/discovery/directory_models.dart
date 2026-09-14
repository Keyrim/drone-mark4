import 'package:equatable/equatable.dart';
import 'package:mark4/gen/mark4.pb.dart';

/// What a node said about itself in its Announce.
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

/// Where the directory stands with a node.
enum DirectoryState {
  /// Asked, no answer yet.
  pending,

  /// The announce is valid.
  known,

  /// Gave up asking.
  mute,
}

/// One node the directory knows of.
class DirectoryEntry extends Equatable {
  const DirectoryEntry({
    required this.id,
    required this.state,
    required this.hops,
    this.announce,
  });

  /// Node id.
  final int id;

  /// Where the directory stands with it.
  final DirectoryState state;

  /// Distance in relays, from the transport's node table.
  final int hops;

  /// What it answered; null until its state is [DirectoryState.known].
  final NodeAnnounce? announce;

  @override
  List<Object?> get props => [id, state, hops, announce];
}

/// Who is around, as the directory holds it: one entry per node the
/// transport hears, in no particular order.
class DirectorySnapshot extends Equatable {
  const DirectorySnapshot({required this.entries});

  static const DirectorySnapshot empty = DirectorySnapshot(entries: []);

  final List<DirectoryEntry> entries;

  /// The entry of [nodeId], null when the directory holds none.
  DirectoryEntry? find(int nodeId) {
    for (final entry in entries) {
      if (entry.id == nodeId) {
        return entry;
      }
    }
    return null;
  }

  /// The known entries whose kind is one of [kinds], in directory order.
  List<DirectoryEntry> nodesOfKind(List<NodeKind> kinds) => [
    for (final entry in entries)
      if (entry.state == DirectoryState.known &&
          kinds.contains(entry.announce?.kind))
        entry,
  ];

  @override
  List<Object?> get props => [entries];
}
