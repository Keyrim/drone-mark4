import 'package:bloc_test/bloc_test.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/app/gamepad_status_bloc/gamepad_status_bloc.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

import '../fakes/bench.dart';

void main() {
  late Bench bench;

  setUp(() async {
    bench = Bench();
    await bench.boot();
  });
  tearDown(() => bench.dispose());

  blocTest<GamepadStatusBloc, GamepadStatusState>(
    'follows the controller coming and going',
    build: () => GamepadStatusBloc(bench.backend.gamepad),
    act: (bloc) async {
      bloc.add(const GamepadStatusStarted());
      await bench.settle();
      bench.gamepad.devices([
        const GamepadDevice(id: 7, name: 'Xbox', hasRumble: true),
      ]);
      await bench.settle();
      bench.gamepad.devices([]);
      await bench.settle();
    },
    // The first value is the current one, restated when the bloc starts.
    expect: () => [
      const GamepadStatusState(),
      const GamepadStatusState(connected: true),
      const GamepadStatusState(),
    ],
  );
}
