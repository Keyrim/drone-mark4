import 'dart:typed_data';

import 'package:equatable/equatable.dart';

/// One game controller the phone sees.
class GamepadDevice extends Equatable {
  const GamepadDevice({
    required this.id,
    required this.name,
    required this.hasRumble,
  });

  /// Android's input device id; changes when the controller reconnects.
  final int id;
  final String name;

  /// The controller has a vibrator the phone can drive.
  final bool hasRumble;

  @override
  List<Object?> get props => [id, name, hasRumble];
}

/// The buttons of one sample as a bit mask, one bit per button. The bits
/// mirror GamepadBridge.kt; the hat of a controller that reports its D-pad
/// as an axis pair is folded into the four D-pad bits by the decoder.
class GamepadButtons extends Equatable {
  const GamepadButtons(this.mask);

  static const GamepadButtons none = GamepadButtons(0);

  static const int bitA = 0;
  static const int bitB = 1;
  static const int bitX = 2;
  static const int bitY = 3;
  static const int bitLb = 4;
  static const int bitRb = 5;
  static const int bitView = 6;
  static const int bitMenu = 7;
  static const int bitLeftThumb = 8;
  static const int bitRightThumb = 9;
  static const int bitDpadUp = 10;
  static const int bitDpadDown = 11;
  static const int bitDpadLeft = 12;
  static const int bitDpadRight = 13;
  static const int bitGuide = 14;

  /// The names of the bits, by bit, for a display.
  static const List<String> names = [
    'A',
    'B',
    'X',
    'Y',
    'LB',
    'RB',
    'View',
    'Menu',
    'LS',
    'RS',
    'Up',
    'Down',
    'Left',
    'Right',
    'Xbox',
  ];

  final int mask;

  bool isDown(int bit) => (mask >> bit) & 1 == 1;

  bool get a => isDown(bitA);
  bool get b => isDown(bitB);
  bool get x => isDown(bitX);
  bool get y => isDown(bitY);
  bool get lb => isDown(bitLb);
  bool get rb => isDown(bitRb);
  bool get view => isDown(bitView);
  bool get menu => isDown(bitMenu);
  bool get leftThumb => isDown(bitLeftThumb);
  bool get rightThumb => isDown(bitRightThumb);
  bool get dpadUp => isDown(bitDpadUp);
  bool get dpadDown => isDown(bitDpadDown);
  bool get dpadLeft => isDown(bitDpadLeft);
  bool get dpadRight => isDown(bitDpadRight);
  bool get guide => isDown(bitGuide);

  GamepadButtons withBit(int bit, {required bool down}) =>
      GamepadButtons(down ? mask | (1 << bit) : mask & ~(1 << bit));

  @override
  List<Object?> get props => [mask];
}

/// One report of a controller: where every axis is and which buttons are
/// down, stamped by the kernel when the Bluetooth stack handed it over.
/// Axes are in Android's convention: sticks in [-1, 1] with right and down
/// positive, triggers in [0, 1]. The mapping onto the pilot's sticks is not
/// this class's business.
class GamepadSample extends Equatable {
  const GamepadSample({
    required this.deviceId,
    required this.eventTimeMs,
    this.leftX = 0,
    this.leftY = 0,
    this.rightX = 0,
    this.rightY = 0,
    this.leftTrigger = 0,
    this.rightTrigger = 0,
    this.buttons = GamepadButtons.none,
  });

  /// Length of a packed sample, the layout GamepadBridge.kt writes.
  static const int packedLength = 11;

  /// Hat deflection past which the D-pad counts as pressed.
  static const double hatThreshold = 0.5;

  /// Decodes one packed sample; null when the length is not the layout's.
  static GamepadSample? fromPacked(Float64List packed) {
    if (packed.length != packedLength) {
      return null;
    }
    var buttons = GamepadButtons(packed[10].toInt());
    final hatX = packed[8];
    final hatY = packed[9];
    // A hat reported as axes is the D-pad: fold it into the button bits so
    // a consumer reads one thing whatever the controller does.
    if (hatX.abs() > hatThreshold || hatY.abs() > hatThreshold) {
      buttons = buttons
          .withBit(GamepadButtons.bitDpadLeft, down: hatX < -hatThreshold)
          .withBit(GamepadButtons.bitDpadRight, down: hatX > hatThreshold)
          .withBit(GamepadButtons.bitDpadUp, down: hatY < -hatThreshold)
          .withBit(GamepadButtons.bitDpadDown, down: hatY > hatThreshold);
    }
    return GamepadSample(
      deviceId: packed[0].toInt(),
      eventTimeMs: packed[1],
      leftX: packed[2],
      leftY: packed[3],
      rightX: packed[4],
      rightY: packed[5],
      leftTrigger: packed[6],
      rightTrigger: packed[7],
      buttons: buttons,
    );
  }

  final int deviceId;

  /// Kernel timestamp of the report, Android uptime base [ms].
  final double eventTimeMs;
  final double leftX;
  final double leftY;
  final double rightX;
  final double rightY;
  final double leftTrigger;
  final double rightTrigger;
  final GamepadButtons buttons;

  @override
  List<Object?> get props => [
    deviceId,
    eventTimeMs,
    leftX,
    leftY,
    rightX,
    rightY,
    leftTrigger,
    rightTrigger,
    buttons,
  ];
}

/// What the gamepad manager knows: the controllers present, the last report
/// and how fast reports come in.
class GamepadState extends Equatable {
  const GamepadState({
    this.devices = const [],
    this.sample,
    this.sampleCount = 0,
    this.reportHz = 0,
  });

  static const GamepadState empty = GamepadState();

  /// The controllers the phone sees, in Android's order.
  final List<GamepadDevice> devices;

  /// The last report, null before the first one or after its device left.
  final GamepadSample? sample;

  /// Reports received since boot.
  final int sampleCount;

  /// Report rate over the last reports [Hz], 0 until there are enough.
  final double reportHz;

  /// A controller is present.
  bool get connected => devices.isNotEmpty;

  /// The controller of the last report, null when it left or none reported.
  GamepadDevice? get active {
    final id = sample?.deviceId;
    if (id == null) {
      return null;
    }
    for (final device in devices) {
      if (device.id == id) {
        return device;
      }
    }
    return null;
  }

  GamepadState copyWith({
    List<GamepadDevice>? devices,
    GamepadSample? sample,
    bool clearSample = false,
    int? sampleCount,
    double? reportHz,
  }) => GamepadState(
    devices: devices ?? this.devices,
    sample: clearSample ? null : sample ?? this.sample,
    sampleCount: sampleCount ?? this.sampleCount,
    reportHz: reportHz ?? this.reportHz,
  );

  @override
  List<Object?> get props => [devices, sample, sampleCount, reportHz];
}
