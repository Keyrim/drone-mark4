import 'package:bloc_test/bloc_test.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';
import 'package:mark4/pages/gamepad/gamepad_bloc.dart';

import '../fakes/bench.dart';

const GamepadDevice xbox = GamepadDevice(id: 7, name: 'Xbox', hasRumble: true);

void main() {
  late Bench bench;

  setUp(() async {
    bench = Bench();
    await bench.boot();
  });
  tearDown(() => bench.dispose());

  blocTest<GamepadBloc, GamepadPageState>(
    'follows the manager and reports the haptics',
    build: () => GamepadBloc(bench.backend.gamepad),
    act: (bloc) async {
      bloc.add(const GamepadPageStarted());
      await Future<void>.delayed(Duration.zero);
      bench.gamepad.devices([xbox]);
      await bench.settle();
      bloc.add(const GamepadPageRumbleRequested());
      await bench.settle();
    },
    expect: () => [
      const GamepadPageState(),
      const GamepadPageState(gamepad: GamepadState(devices: [xbox])),
      const GamepadPageState(
        gamepad: GamepadState(devices: [xbox]),
        lastHaptic: HapticOutcome.done,
      ),
    ],
    verify: (_) => expect(bench.gamepad.rumbles, hasLength(1)),
  );

  blocTest<GamepadBloc, GamepadPageState>(
    'a rumble without a controller is reported as failed',
    build: () => GamepadBloc(bench.backend.gamepad),
    act: (bloc) => bloc.add(const GamepadPageRumbleRequested()),
    expect: () => [const GamepadPageState(lastHaptic: HapticOutcome.failed)],
  );
}
