import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/gen/mark4.pb.dart';

import '../fakes/bench.dart';

const int simId = 0x11111111;
const int boardId = 0x22222222;
const int hubId = 0x33333333;

void main() {
  late Bench bench;

  setUp(() async {
    bench = Bench();
    await bench.boot();
  });
  tearDown(() => bench.dispose());

  test(
    'the roster lists the drones, sorted by id, and counts the others',
    () async {
      bench.node.announceDrone(boardId, 'mark4', NodeKind.FIRMWARE, 1000);
      bench.node.announceDrone(simId, 'drone_sim', NodeKind.DRONE_SIM, 1000);
      bench.node.announceDrone(hubId, 'hub-laptop', NodeKind.GATEWAY, 1000);
      bench.node.hear(0x44444444, 1000); // heard, not announced yet
      await bench.poll();
      final roster = bench.backend.drones.roster.value;
      expect(roster.drones.map((d) => d.nodeId), [simId, boardId]);
      expect(roster.drones.first.name, 'drone_sim');
      expect(roster.otherNodeCount, 2);
    },
  );

  test(
    'connect follows the drone: connected while heard, lost when silent, back on its own',
    () async {
      final drones = bench.backend.drones;
      bench.node.announceDrone(simId, 'drone_sim', NodeKind.DRONE_SIM, 1000);
      await bench.poll();

      await drones.connect(simId);
      var connection = drones.connection.value;
      expect(connection.status, DroneLinkStatus.connected);
      expect(connection.nodeId, simId);
      expect(connection.info?.announce.name, 'drone_sim');
      expect(connection.info?.address, '192.168.4.1:47821');

      bench.node.forget(simId);
      await bench.poll();
      connection = drones.connection.value;
      expect(connection.status, DroneLinkStatus.lost);
      expect(
        connection.info?.announce.name,
        'drone_sim',
        reason: 'last known state kept',
      );

      bench.node.announceDrone(
        simId,
        'drone_sim',
        NodeKind.DRONE_SIM,
        bench.nowUs,
      );
      await bench.poll();
      expect(drones.connection.value.status, DroneLinkStatus.connected);
    },
  );

  test('connecting to a drone never heard is lost without info', () async {
    await bench.backend.drones.connect(boardId);
    final connection = bench.backend.drones.connection.value;
    expect(connection.status, DroneLinkStatus.lost);
    expect(connection.info, isNull);
  });

  test('disconnect forgets the drone', () async {
    final drones = bench.backend.drones;
    bench.node.announceDrone(simId, 'drone_sim', NodeKind.DRONE_SIM, 1000);
    await bench.poll();
    await drones.connect(simId);
    await drones.disconnect();
    expect(drones.connection.value, DroneConnection.none);
    await bench.poll();
    expect(drones.connection.value, DroneConnection.none);
  });

  test('the connection stream emits only on change', () async {
    final drones = bench.backend.drones;
    final statuses = <DroneLinkStatus>[];
    final subscription = drones.connection.listen(
      (c) => statuses.add(c.status),
    );
    bench.node.announceDrone(simId, 'drone_sim', NodeKind.DRONE_SIM, 1000);
    await bench.poll();
    await drones.connect(simId);
    await bench.poll(); // no change
    bench.node.forget(simId);
    await bench.poll();
    await bench.poll(); // still lost
    await Future<void>.delayed(Duration.zero);
    expect(statuses, [
      DroneLinkStatus.none,
      DroneLinkStatus.lost, // connect(), before the table is read
      DroneLinkStatus.connected,
      DroneLinkStatus.lost,
    ]);
    await subscription.cancel();
  });
}
