import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/gamepad/abs_gamepad_source.dart';
import 'package:mark4/back/gamepad/android_gamepad_source.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

/// A packed sample as GamepadBridge.kt writes it.
Float64List packed({
  double deviceId = 3,
  double eventTimeMs = 1000,
  double leftX = 0,
  double leftY = 0,
  double rightX = 0,
  double rightY = 0,
  double leftTrigger = 0,
  double rightTrigger = 0,
  double hatX = 0,
  double hatY = 0,
  double buttons = 0,
}) => Float64List.fromList([
  deviceId,
  eventTimeMs,
  leftX,
  leftY,
  rightX,
  rightY,
  leftTrigger,
  rightTrigger,
  hatX,
  hatY,
  buttons,
]);

void main() {
  test('a packed sample decodes field by field', () {
    final sample = GamepadSample.fromPacked(
      packed(
        deviceId: 9,
        eventTimeMs: 2500.5,
        leftX: -0.25,
        leftY: 0.5,
        rightX: 1,
        rightY: -1,
        leftTrigger: 0.1,
        rightTrigger: 0.9,
        buttons: (1 << GamepadButtons.bitA | 1 << GamepadButtons.bitRb)
            .toDouble(),
      ),
    );
    expect(sample, isNotNull);
    expect(sample!.deviceId, 9);
    expect(sample.eventTimeMs, 2500.5);
    expect(sample.leftX, -0.25);
    expect(sample.leftY, 0.5);
    expect(sample.rightX, 1);
    expect(sample.rightY, -1);
    expect(sample.leftTrigger, 0.1);
    expect(sample.rightTrigger, 0.9);
    expect(sample.buttons.a, isTrue);
    expect(sample.buttons.rb, isTrue);
    expect(sample.buttons.b, isFalse);
    expect(sample.buttons.dpadUp, isFalse);
  });

  test('a hat reported as axes becomes the D-pad bits', () {
    final up = GamepadSample.fromPacked(packed(hatY: -1))!;
    expect(up.buttons.dpadUp, isTrue);
    expect(up.buttons.dpadDown, isFalse);
    final downRight = GamepadSample.fromPacked(packed(hatX: 1, hatY: 1))!;
    expect(downRight.buttons.dpadDown, isTrue);
    expect(downRight.buttons.dpadRight, isTrue);
    expect(downRight.buttons.dpadLeft, isFalse);
    // A centred hat leaves the key-driven bits alone.
    final keyed = GamepadSample.fromPacked(
      packed(buttons: (1 << GamepadButtons.bitDpadLeft).toDouble()),
    )!;
    expect(keyed.buttons.dpadLeft, isTrue);
  });

  test('a packed sample of the wrong length is refused', () {
    expect(GamepadSample.fromPacked(Float64List(10)), isNull);
    expect(GamepadSample.fromPacked(Float64List(12)), isNull);
  });

  test('the button names cover every bit once', () {
    expect(GamepadButtons.names.length, GamepadButtons.bitGuide + 1);
    expect(GamepadButtons.names.toSet().length, GamepadButtons.names.length);
    const all = GamepadButtons(0x7FFF);
    for (var bit = 0; bit <= GamepadButtons.bitGuide; ++bit) {
      expect(all.isDown(bit), isTrue);
    }
    expect(all.withBit(GamepadButtons.bitB, down: false).b, isFalse);
  });

  test('the channel decoder tells samples from device lists', () {
    final sample = AndroidGamepadSource.decodeEvent(packed(rightTrigger: 1));
    expect(sample, isA<GamepadSampleEvent>());
    expect((sample! as GamepadSampleEvent).sample.rightTrigger, 1);

    final devices = AndroidGamepadSource.decodeEvent([
      {'id': 4, 'name': 'Xbox Wireless Controller', 'rumble': true},
      {'id': 5, 'name': 'Other', 'rumble': false},
      'garbage',
    ]);
    expect(devices, isA<GamepadDevicesEvent>());
    expect((devices! as GamepadDevicesEvent).devices, const [
      GamepadDevice(id: 4, name: 'Xbox Wireless Controller', hasRumble: true),
      GamepadDevice(id: 5, name: 'Other', hasRumble: false),
    ]);

    expect(AndroidGamepadSource.decodeEvent('what'), isNull);
    expect(AndroidGamepadSource.decodeEvent(Float64List(3)), isNull);
  });

  test('the active device is the one of the last report', () {
    const pad = GamepadDevice(id: 1, name: 'pad', hasRumble: false);
    const state = GamepadState(
      devices: [pad],
      sample: GamepadSample(deviceId: 1, eventTimeMs: 0),
    );
    expect(state.active, pad);
    expect(state.connected, isTrue);
    expect(
      state
          .copyWith(sample: const GamepadSample(deviceId: 2, eventTimeMs: 0))
          .active,
      isNull,
    );
    expect(GamepadState.empty.connected, isFalse);
  });
}
