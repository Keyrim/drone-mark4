import 'package:equatable/equatable.dart';
import 'package:mark4/back/drone/drone_models.dart';

sealed class DroneEvent extends Equatable {
  const DroneEvent();

  @override
  List<Object?> get props => [];
}

/// Connect to the page's drone and follow the link.
final class DroneStarted extends DroneEvent {
  const DroneStarted();
}

/// The link to the drone changed.
final class DroneConnectionChanged extends DroneEvent {
  const DroneConnectionChanged(this.connection);

  final DroneConnection connection;

  @override
  List<Object?> get props => [connection];
}
