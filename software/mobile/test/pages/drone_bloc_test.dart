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

  DroneBloc build() => DroneBloc(
    drones: bench.backend.drones,
    pilot: bench.backend.pilot,
    transport: bench.backend.transport,
    nodeId: simId,
  );

  blocTest<DroneBloc, DroneState>(
    'connects and engages on start, reports the loss',
    build: build,
    act: (bloc) async {
      bloc.add(const DroneStarted());
      await bench.settle();
      await bench.settle();
      bench.node.forget(simId);
      await bench.poll();
      await bench.settle();
    },
    // bloc_test closes the bloc before verify, which disengages the pilot:
    // the bloc's own last state is what says the cockpit was engaged.
    verify: (bloc) {
      expect(bloc.state.nodeId, simId);
      expect(bloc.state.isConnected, isFalse);
      expect(bloc.state.connection.status, DroneLinkStatus.lost);
      expect(bloc.state.connection.info?.announce.name, 'drone_sim');
      expect(bloc.state.pilot.engaged, isTrue);
      expect(bloc.state.pilot.killed, isTrue);
    },
  );

  blocTest<DroneBloc, DroneState>(
    'follows the Status of the drone',
    build: build,
    act: (bloc) async {
      bloc.add(const DroneStarted());
      await bench.settle();
      bench.node.receive(
        simId,
        Envelope()
          ..status = (Status()
            ..flightPhase = FlightPhase.PHASE_IDLE
            ..imuValid = true
            ..baroValid = true
            ..rcLinkOk = true),
      );
      await bench.poll();
      await bench.settle();
    },
    verify: (bloc) {
      expect(bloc.state.status?.phase, FlightPhase.PHASE_IDLE);
      expect(bloc.state.status?.rcLinkOk, isTrue);
      expect(bloc.state.statusFresh, isTrue);
      // The ages follow the snapshot clock, which ticks at its own cadence.
      expect(bloc.state.nowUs, lessThanOrEqualTo(bench.nowUs));
      expect(bloc.state.nowUs, greaterThan(0));
    },
  );

  test('closing the bloc disengages and disconnects', () async {
    final bloc = build()..add(const DroneStarted());
    await bench.settle();
    await bench.settle();
    expect(
      bench.backend.drones.connection.value.status,
      DroneLinkStatus.connected,
    );
    expect(bench.backend.pilot.state.value.engaged, isTrue);
    await bloc.close();
    expect(bench.backend.drones.connection.value, DroneConnection.none);
    expect(bench.backend.pilot.state.value.engaged, isFalse);
  });

  test('the background kills', () async {
    final bloc = build()..add(const DroneStarted());
    await bench.settle();
    await bench.settle();
    bloc.add(const DroneBackgrounded());
    await bench.settle();
    expect(bench.backend.pilot.state.value.killed, isTrue);
    await bloc.close();
  });
}
