import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:mark4/gen/wire_hash.dart';

import '../fakes/bench.dart';
import '../fakes/fake_platform.dart';
import '../fakes/fake_transport_node.dart';

void main() {
  late Bench bench;

  setUp(() => bench = Bench());
  tearDown(() => bench.dispose());

  test('boot takes the multicast lock and beacons a PHONE announce', () async {
    await bench.boot();
    expect(bench.platform.lockAcquired, 1);
    final beacon = Envelope.fromBuffer(bench.node.beacon!);
    expect(beacon.hasAnnounce(), isTrue);
    expect(beacon.announce.kind, NodeKind.PHONE);
    expect(beacon.announce.name, 'Theo phone');
    expect(beacon.announce.wireHash, wireHash);
    expect(bench.backend.transport.identity.value.nodeId, phoneNodeId);
    expect(bench.backend.transport.identity.value.name, 'Theo phone');
  });

  test('a refused multicast lock is not fatal', () async {
    bench = Bench(platform: FakePlatform(lockGranted: false));
    await bench.boot();
    expect(bench.node.beacon, isNotNull);
  });

  test('the announce name is ASCII and at most 16 characters', () async {
    expect(TransportManager.announceName('Pixel 8 Pro'), 'Pixel 8 Pro');
    expect(
      TransportManager.announceName('Telephone de Theo Magne'),
      'Telephone de The',
    );
    expect(TransportManager.announceName('café ☃'), 'caf');
    expect(TransportManager.announceName('☃☃'), 'phone');
  });

  test(
    'a node appears with its announce and leaves when the transport forgets it',
    () async {
      await bench.boot();
      final transport = bench.backend.transport;
      expect(transport.snapshots.value.nodes, isEmpty);

      bench.node.announceDrone(
        0x11111111,
        'drone_sim',
        NodeKind.DRONE_SIM,
        10000,
        wireHash: wireHash,
      );
      await bench.poll();
      final snapshot = transport.snapshots.value;
      expect(snapshot.nodes, hasLength(1));
      expect(snapshot.node(0x11111111)?.announce?.name, 'drone_sim');
      expect(snapshot.node(0x11111111)?.announce?.wireMismatch, isFalse);
      expect(snapshot.node(0x11111111)?.info.address, '192.168.4.1:47821');

      bench.node.forget(0x11111111);
      await bench.poll();
      expect(transport.snapshots.value.nodes, isEmpty);
    },
  );

  test('a node built on another schema is flagged', () async {
    await bench.boot();
    bench.node.announceDrone(
      0x22222222,
      'old',
      NodeKind.FIRMWARE,
      10000,
      wireHash: 0x12345678,
    );
    await bench.poll();
    expect(
      bench.backend.transport.snapshots.value
          .node(0x22222222)
          ?.announce
          ?.wireMismatch,
      isTrue,
    );
  });

  test(
    'snapshots are emitted on a table change, then at the snapshot period',
    () async {
      await bench.boot();
      final transport = bench.backend.transport;
      final emitted = <int>[];
      final subscription = transport.snapshots
          .skip(1)
          .listen((s) => emitted.add(s.nowUs));

      bench.node.hear(0x33333333, 10000);
      await bench.poll(advanceUs: 10000); // table changed: emitted
      await bench.poll(
        advanceUs: 10000,
      ); // nothing new, 10 ms later: not emitted
      await bench.poll(advanceUs: 490000); // 500 ms since the last one: emitted
      await Future<void>.delayed(Duration.zero);
      expect(emitted, [10000, 510000]);
      await subscription.cancel();
    },
  );

  test(
    'every envelope for this node is published, garbage is counted',
    () async {
      await bench.boot();
      final transport = bench.backend.transport;
      final received = <int>[];
      final subscription = transport.envelopes.listen(
        (e) => received.add(e.src),
      );

      bench.node.receive(0x44444444, Envelope()..status = Status());
      bench.node.queue.add(InboundPayloadGarbage.of(0x55555555));
      await bench.poll();
      await Future<void>.delayed(Duration.zero);
      expect(received, [0x44444444]);
      expect(transport.decodeErrors, 1);
      await subscription.cancel();
    },
  );

  test('send goes through the node', () async {
    await bench.boot();
    expect(
      await bench.backend.transport.send(
        0x66666666,
        Envelope()..status = Status(),
      ),
      isTrue,
    );
    expect(bench.node.sent.single.$1, 0x66666666);
  });

  test('dispose closes the node and releases the lock', () async {
    await bench.boot();
    await bench.dispose();
    expect(bench.node.disposed, isTrue);
    expect(bench.platform.lockReleased, 1);
    bench = Bench(); // so tearDown disposes a live one
  });
}
