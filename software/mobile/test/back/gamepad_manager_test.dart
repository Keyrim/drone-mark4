import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

import '../fakes/bench.dart';

const GamepadDevice xbox = GamepadDevice(
  id: 7,
  name: 'Xbox Wireless Controller',
  hasRumble: true,
);

GamepadSample report(double timeMs, {double rightTrigger = 0}) =>
    GamepadSample(deviceId: 7, eventTimeMs: timeMs, rightTrigger: rightTrigger);

void main() {
  group('with an immediate state', () {
    late Bench bench;

    setUp(() async {
      bench = Bench();
      await bench.boot();
    });
    tearDown(() => bench.dispose());

    test('the device list is the state, connected when not empty', () async {
      final gamepad = bench.backend.gamepad;
      expect(gamepad.state.value.connected, isFalse);
      bench.gamepad.devices([xbox]);
      await bench.settle();
      expect(gamepad.state.value.devices, [xbox]);
      expect(gamepad.state.value.connected, isTrue);
      bench.gamepad.devices([]);
      await bench.settle();
      expect(gamepad.state.value.connected, isFalse);
    });

    test(
      'reports reach the samples stream raw and the state counts them',
      () async {
        final gamepad = bench.backend.gamepad;
        final seen = <GamepadSample>[];
        final subscription = gamepad.samples.listen(seen.add);
        bench.gamepad.devices([xbox]);
        bench.gamepad.report(report(1000, rightTrigger: 0.5));
        bench.gamepad.report(report(1008, rightTrigger: 0.6));
        await bench.settle();
        expect(seen.map((s) => s.rightTrigger), [0.5, 0.6]);
        expect(gamepad.state.value.sampleCount, 2);
        expect(gamepad.state.value.sample?.rightTrigger, 0.6);
        expect(gamepad.state.value.active, xbox);
        await subscription.cancel();
      },
    );

    test('the report rate is measured on the controller timestamps', () async {
      final gamepad = bench.backend.gamepad;
      bench.gamepad.devices([xbox]);
      for (var i = 0; i < 40; ++i) {
        bench.gamepad.report(report(1000 + i * 8));
      }
      await bench.settle();
      expect(gamepad.state.value.reportHz, closeTo(125, 0.5));
    });

    test('a controller leaving takes its last report with it', () async {
      final gamepad = bench.backend.gamepad;
      bench.gamepad.devices([xbox]);
      bench.gamepad.report(report(1000, rightTrigger: 1));
      await bench.settle();
      expect(gamepad.state.value.sample, isNotNull);
      bench.gamepad.devices([]);
      await bench.settle();
      expect(gamepad.state.value.sample, isNull);
      expect(gamepad.state.value.reportHz, 0);
      expect(gamepad.state.value.active, isNull);
    });

    test(
      'rumble goes to the active controller, or fails without one',
      () async {
        final gamepad = bench.backend.gamepad;
        expect(await gamepad.rumble(), isFalse);
        bench.gamepad.devices([xbox]);
        await bench.settle();
        expect(await gamepad.rumble(), isTrue);
        expect(bench.gamepad.rumbles.single.$1, xbox.id);
        expect(await gamepad.vibratePhone(), isTrue);
        expect(bench.gamepad.phoneVibrations, hasLength(1));
      },
    );

    test('a controller without rumble is not asked', () async {
      final gamepad = bench.backend.gamepad;
      bench.gamepad.devices([
        const GamepadDevice(id: 2, name: 'cheap', hasRumble: false),
      ]);
      await bench.settle();
      expect(await gamepad.rumble(), isFalse);
      expect(bench.gamepad.rumbles, isEmpty);
    });
  });

  group('with a throttled state', () {
    late Bench bench;

    setUp(() async {
      bench = Bench(gamepadStatePeriod: const Duration(milliseconds: 50));
      await bench.boot();
    });
    tearDown(() => bench.dispose());

    test(
      'the state publishes the first report, then at most once per period, then the trailing one',
      () async {
        final gamepad = bench.backend.gamepad;
        final published = <double?>[];
        final subscription = gamepad.state.listen(
          (state) => published.add(state.sample?.rightTrigger),
        );
        bench.gamepad.devices([xbox]);
        // 125 Hz for 40 ms: one publication at the first report, none after.
        for (var i = 0; i < 5; ++i) {
          bench.gamepad.report(report(1000 + i * 8, rightTrigger: i / 10));
        }
        await bench.settle();
        expect(published, [null, null, 0.0]);

        // The trailing timer publishes the last held report.
        await Future<void>.delayed(const Duration(milliseconds: 80));
        expect(published.last, 0.4);
        await subscription.cancel();
      },
    );
  });
}
