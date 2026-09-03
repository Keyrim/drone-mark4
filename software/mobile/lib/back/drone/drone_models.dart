import 'package:equatable/equatable.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/gen/mark4.pbenum.dart';

/// One drone on the network, what the home list shows.
class DroneSummary extends Equatable {
  const DroneSummary({
    required this.nodeId,
    required this.name,
    required this.kind,
  });

  final int nodeId;
  final String name;
  final NodeKind kind;

  @override
  List<Object?> get props => [nodeId, name, kind];
}

/// Every drone heard, plus how many other nodes share the network.
class DroneRoster extends Equatable {
  const DroneRoster({required this.drones, required this.otherNodeCount});

  static const DroneRoster empty = DroneRoster(drones: [], otherNodeCount: 0);

  final List<DroneSummary> drones;
  final int otherNodeCount;

  @override
  List<Object?> get props => [drones, otherNodeCount];
}

/// Everything known about the connected drone.
class DroneInfo extends Equatable {
  const DroneInfo({
    required this.announce,
    required this.address,
    required this.received,
    required this.lost,
    required this.duplicates,
    required this.lastSeenAgo,
  });

  /// From one node of a snapshot.
  factory DroneInfo.fromNode(
    TransportNode node,
    NodeAnnounce announce,
    int nowUs,
  ) => DroneInfo(
    announce: announce,
    address: node.info.address,
    received: node.info.received,
    lost: node.info.lost,
    duplicates: node.info.duplicates,
    lastSeenAgo: Duration(microseconds: nowUs - node.info.lastSeenUs),
  );

  final NodeAnnounce announce;
  final String address;
  final int received;
  final int lost;
  final int duplicates;
  final Duration lastSeenAgo;

  @override
  List<Object?> get props => [
    announce,
    address,
    received,
    lost,
    duplicates,
    lastSeenAgo,
  ];
}

/// Whether the connected drone is heard right now.
enum DroneLinkStatus {
  /// No drone is connected.
  none,

  /// The drone is in the transport's table.
  connected,

  /// The drone was connected and has been silent for too long, or has not
  /// been heard yet.
  lost,
}

/// The link to the drone the user connected to. Connecting is choosing a node
/// id and following it: the transport is connectionless, the drone is
/// connected while it is heard and lost while it is not, and comes back on
/// its own.
class DroneConnection extends Equatable {
  const DroneConnection({
    required this.status,
    required this.nodeId,
    this.info,
  });

  static const DroneConnection none = DroneConnection(
    status: DroneLinkStatus.none,
    nodeId: 0,
  );

  final DroneLinkStatus status;

  /// 0 when none.
  final int nodeId;

  /// Last known state of the drone, kept while it is lost; null when it was
  /// never heard.
  final DroneInfo? info;

  @override
  List<Object?> get props => [status, nodeId, info];
}
