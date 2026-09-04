import 'package:equatable/equatable.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';

final class HomeState extends Equatable {
  const HomeState({
    this.roster = DroneRoster.empty,
    this.identity = TransportIdentity.none,
  });

  final DroneRoster roster;
  final TransportIdentity identity;

  HomeState copyWith({DroneRoster? roster, TransportIdentity? identity}) =>
      HomeState(
        roster: roster ?? this.roster,
        identity: identity ?? this.identity,
      );

  @override
  List<Object?> get props => [roster, identity];
}
