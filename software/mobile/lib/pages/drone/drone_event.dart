import 'package:equatable/equatable.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/pilot/pilot_models.dart';
import 'package:mark4/gen/mark4.pbenum.dart';

sealed class DroneEvent extends Equatable {
  const DroneEvent();

  @override
  List<Object?> get props => [];
}

/// Connect to the page's drone, engage the transmitter, follow everything.
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

/// A Status of the drone arrived (or was cleared).
final class DroneStatusChanged extends DroneEvent {
  const DroneStatusChanged(this.status);

  final DroneStatus? status;

  @override
  List<Object?> get props => [status];
}

/// The transmitter changed.
final class DronePilotChanged extends DroneEvent {
  const DronePilotChanged(this.pilot);

  final PilotState pilot;

  @override
  List<Object?> get props => [pilot];
}

/// The clock ticked: the ages on screen move.
final class DroneTicked extends DroneEvent {
  const DroneTicked(this.nowUs);

  final int nowUs;

  @override
  List<Object?> get props => [nowUs];
}

/// The user picked a mode on the screen (the D-pad does the same).
final class DroneModeSelected extends DroneEvent {
  const DroneModeSelected(this.mode);

  final RcMode mode;

  @override
  List<Object?> get props => [mode];
}

/// The app left the foreground: a pilot who cannot see the drone kills.
final class DroneBackgrounded extends DroneEvent {
  const DroneBackgrounded();
}
