import 'package:bloc_test/bloc_test.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';
import 'package:mark4/back/transport/transport_snapshot.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:mark4/pages/home/home_bloc.dart';

import '../fakes/bench.dart';

void main() {
  late Bench bench;

  setUp(() async {
    bench = Bench();
    await bench.boot();
  });
  tearDown(() => bench.dispose());

  blocTest<HomeBloc, HomeState>(
    'starts with the phone identity and follows the roster',
    build: () => HomeBloc(
      drones: bench.backend.drones,
      transport: bench.backend.transport,
      gamepad: bench.backend.gamepad,
    ),
    act: (bloc) async {
      bloc.add(const HomeStarted());
      await Future<void>.delayed(Duration.zero);
      bench.node.announceDrone(
        0x11111111,
        'drone_sim',
        NodeKind.DRONE_SIM,
        1000,
      );
      await bench.poll();
    },
    expect: () => [
      const HomeState(
        identity: TransportIdentity(nodeId: phoneNodeId, name: 'Theo phone'),
      ),
      const HomeState(
        identity: TransportIdentity(nodeId: phoneNodeId, name: 'Theo phone'),
        roster: DroneRoster(
          drones: [
            DroneSummary(
              nodeId: 0x11111111,
              name: 'drone_sim',
              kind: NodeKind.DRONE_SIM,
            ),
          ],
          otherNodeCount: 0,
        ),
      ),
    ],
  );

  blocTest<HomeBloc, HomeState>(
    'lights the gamepad action when a controller is present',
    build: () => HomeBloc(
      drones: bench.backend.drones,
      transport: bench.backend.transport,
      gamepad: bench.backend.gamepad,
    ),
    act: (bloc) async {
      bloc.add(const HomeStarted());
      await Future<void>.delayed(Duration.zero);
      bench.gamepad.devices([
        const GamepadDevice(id: 7, name: 'Xbox', hasRumble: true),
      ]);
      await bench.settle();
      bench.gamepad.devices([]);
      await bench.settle();
    },
    expect: () => [
      const HomeState(
        identity: TransportIdentity(nodeId: phoneNodeId, name: 'Theo phone'),
      ),
      const HomeState(
        identity: TransportIdentity(nodeId: phoneNodeId, name: 'Theo phone'),
        gamepadConnected: true,
      ),
      const HomeState(
        identity: TransportIdentity(nodeId: phoneNodeId, name: 'Theo phone'),
      ),
    ],
  );
}
