import 'package:bloc_test/bloc_test.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:mark4/pages/drone/drone_bloc.dart';

import '../fakes/bench.dart';

const int simId = 0x11111111;

void main() {
  late Bench bench;

  setUp(() async {
    bench = Bench();
    await bench.boot();
    bench.node.announceDrone(simId, 'drone_sim', NodeKind.DRONE_SIM, 1000);
    await bench.poll();
  });
  tearDown(() => bench.dispose());

  blocTest<DroneBloc, DroneState>(
    'connects on start, reports the loss, disconnects on close',
    build: () => DroneBloc(drones: bench.backend.drones, nodeId: simId),
    act: (bloc) async {
      bloc.add(const DroneStarted());
      await Future<void>.delayed(Duration.zero);
      bench.node.forget(simId);
      await bench.poll();
    },
    verify: (bloc) {
      expect(bloc.state.nodeId, simId);
      expect(bloc.state.isConnected, isFalse);
      expect(bloc.state.connection.status, DroneLinkStatus.lost);
      expect(bloc.state.connection.info?.announce.name, 'drone_sim');
    },
    expect: () => [
      isA<DroneState>().having(
        (s) => s.connection.status,
        'status',
        DroneLinkStatus.lost,
      ),
      isA<DroneState>().having(
        (s) => s.connection.status,
        'status',
        DroneLinkStatus.connected,
      ),
      isA<DroneState>().having(
        (s) => s.connection.status,
        'status',
        DroneLinkStatus.lost,
      ),
    ],
  );

  test('closing the bloc disconnects', () async {
    final bloc = DroneBloc(drones: bench.backend.drones, nodeId: simId)
      ..add(const DroneStarted());
    await Future<void>.delayed(Duration.zero);
    expect(
      bench.backend.drones.connection.value.status,
      DroneLinkStatus.connected,
    );
    await bloc.close();
    expect(bench.backend.drones.connection.value, DroneConnection.none);
  });
}
