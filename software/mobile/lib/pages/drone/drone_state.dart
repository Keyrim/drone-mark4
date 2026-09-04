import 'package:equatable/equatable.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/pilot/pilot_models.dart';

final class DroneState extends Equatable {
  const DroneState({
    required this.nodeId,
    this.connection = DroneConnection.none,
    this.status,
    this.pilot = PilotState.idle,
    this.nowUs = 0,
  });

  /// A Status older than this is not what the drone does any more.
  static const Duration statusStaleAfter = Duration(milliseconds: 500);

  final int nodeId;
  final DroneConnection connection;

  /// The last Status heard, null before the first.
  final DroneStatus? status;
  final PilotState pilot;

  /// The clock the ages are measured against, the transport's [us].
  final int nowUs;

  bool get isConnected => connection.status == DroneLinkStatus.connected;

  /// The Status is recent enough to describe the drone now.
  bool get statusFresh {
    final status = this.status;
    return status != null &&
        nowUs - status.receivedAtUs <= statusStaleAfter.inMicroseconds;
  }

  /// Age of the last Status, null before the first.
  Duration? get statusAge {
    final status = this.status;
    return status == null
        ? null
        : Duration(microseconds: nowUs - status.receivedAtUs);
  }

  DroneState copyWith({
    DroneConnection? connection,
    DroneStatus? status,
    bool clearStatus = false,
    PilotState? pilot,
    int? nowUs,
  }) => DroneState(
    nodeId: nodeId,
    connection: connection ?? this.connection,
    status: clearStatus ? null : status ?? this.status,
    pilot: pilot ?? this.pilot,
    nowUs: nowUs ?? this.nowUs,
  );

  @override
  List<Object?> get props => [nodeId, connection, status, pilot, nowUs];
}
