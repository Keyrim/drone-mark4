import 'package:equatable/equatable.dart';
import 'package:mark4/back/drone/drone_models.dart';

final class DroneState extends Equatable {
  const DroneState({
    required this.nodeId,
    this.connection = DroneConnection.none,
  });

  final int nodeId;
  final DroneConnection connection;

  bool get isConnected => connection.status == DroneLinkStatus.connected;

  @override
  List<Object?> get props => [nodeId, connection];
}
