import 'package:equatable/equatable.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';

sealed class HomeEvent extends Equatable {
  const HomeEvent();

  @override
  List<Object?> get props => [];
}

/// Start following the managers.
final class HomeStarted extends HomeEvent {
  const HomeStarted();
}

/// The drones on the network changed.
final class HomeRosterChanged extends HomeEvent {
  const HomeRosterChanged(this.roster);

  final DroneRoster roster;

  @override
  List<Object?> get props => [roster];
}

/// This node's identity changed (once, at boot).
final class HomeIdentityChanged extends HomeEvent {
  const HomeIdentityChanged(this.identity);

  final TransportIdentity identity;

  @override
  List<Object?> get props => [identity];
}

/// A game controller appeared or left.
final class HomeGamepadChanged extends HomeEvent {
  const HomeGamepadChanged({required this.connected});

  final bool connected;

  @override
  List<Object?> get props => [connected];
}
