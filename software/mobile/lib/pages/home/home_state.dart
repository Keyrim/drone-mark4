import 'package:equatable/equatable.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';

final class HomeState extends Equatable {
  const HomeState({
    this.roster = DroneRoster.empty,
    this.identity = TransportIdentity.none,
    this.gamepadConnected = false,
  });

  final DroneRoster roster;
  final TransportIdentity identity;

  /// A game controller is present.
  final bool gamepadConnected;

  HomeState copyWith({
    DroneRoster? roster,
    TransportIdentity? identity,
    bool? gamepadConnected,
  }) => HomeState(
    roster: roster ?? this.roster,
    identity: identity ?? this.identity,
    gamepadConnected: gamepadConnected ?? this.gamepadConnected,
  );

  @override
  List<Object?> get props => [roster, identity, gamepadConnected];
}
