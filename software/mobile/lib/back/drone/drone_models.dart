import 'dart:math' as math;

import 'package:equatable/equatable.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/gen/mark4.pb.dart';

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

/// The last Status report of the connected drone: what it is doing, as the
/// flight core published it, decimated to 50 Hz and always on.
class DroneStatus extends Equatable {
  const DroneStatus({
    required this.receivedAtUs,
    required this.timestampUs,
    required this.phase,
    required this.attitude,
    required this.motors,
    required this.imuValid,
    required this.baroValid,
    required this.rcLinkOk,
    required this.throwCount,
  });

  /// From the wire, stamped with the phone clock it arrived at.
  factory DroneStatus.fromWire(Status status, int receivedAtUs) => DroneStatus(
    receivedAtUs: receivedAtUs,
    timestampUs: status.timestampUs.toInt(),
    phase: status.flightPhase,
    attitude: status.attitudeQuat.length == 4
        ? List.unmodifiable(status.attitudeQuat)
        : const [1, 0, 0, 0],
    motors: status.motor.length == 4
        ? List.unmodifiable(status.motor)
        : const [0, 0, 0, 0],
    imuValid: status.imuValid,
    baroValid: status.baroValid,
    rcLinkOk: status.rcLinkOk,
    throwCount: status.throwCount,
  );

  /// Phone clock at reception [us], the transport's clock.
  final int receivedAtUs;

  /// The drone's acquisition time [us].
  final int timestampUs;
  final FlightPhase phase;

  /// Estimated attitude, w x y z.
  final List<double> attitude;

  /// Normalized motor commands [0, 1].
  final List<double> motors;
  final bool imuValid;
  final bool baroValid;

  /// The drone heard a pilot recently: its RC fail-safe is not active.
  final bool rcLinkOk;
  final int throwCount;

  /// The motors may run in this phase.
  bool get armed =>
      phase != FlightPhase.PHASE_IDLE &&
      phase != FlightPhase.PHASE_CUTOFF &&
      phase != FlightPhase.PHASE_FAULT;

  /// Roll angle of the estimated attitude, positive right [rad].
  double get rollRad {
    final w = attitude[0];
    final x = attitude[1];
    final y = attitude[2];
    final z = attitude[3];
    return math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y));
  }

  /// Pitch angle of the estimated attitude, positive nose down [rad].
  double get pitchRad {
    final w = attitude[0];
    final x = attitude[1];
    final y = attitude[2];
    final z = attitude[3];
    final sinP = (2 * (w * y - z * x)).clamp(-1.0, 1.0);
    return math.asin(sinP);
  }

  @override
  List<Object?> get props => [
    receivedAtUs,
    timestampUs,
    phase,
    attitude,
    motors,
    imuValid,
    baroValid,
    rcLinkOk,
    throwCount,
  ];
}
