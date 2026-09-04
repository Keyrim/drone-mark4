import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:logging/logging.dart';
import 'package:mark4/back/gamepad/abs_gamepad_source.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';

final Logger _log = Logger('back/gamepad');

/// The Android side: the `mark4/gamepad` event channel fed by
/// GamepadBridge.kt (a Float64List per report, a List of Maps per device
/// change) and the haptic methods of the `mark4/platform` channel.
class AndroidGamepadSource implements AbsGamepadSource {
  static const EventChannel _events = EventChannel('mark4/gamepad');
  static const MethodChannel _platform = MethodChannel('mark4/platform');

  @override
  Stream<GamepadEvent> get events => _events
      .receiveBroadcastStream()
      .map(decodeEvent)
      .where((event) => event != null)
      .cast<GamepadEvent>();

  /// Turns one raw channel event into a typed one; null for what is not
  /// understood (logged once per shape, never thrown: a bad event must not
  /// end the stream).
  static GamepadEvent? decodeEvent(Object? raw) {
    if (raw is Float64List) {
      final sample = GamepadSample.fromPacked(raw);
      if (sample == null) {
        _log.warning('gamepad sample of ${raw.length} doubles ignored');
        return null;
      }
      return GamepadSampleEvent(sample);
    }
    if (raw is List) {
      final devices = <GamepadDevice>[];
      for (final entry in raw) {
        if (entry is! Map) {
          continue;
        }
        final id = entry['id'];
        final name = entry['name'];
        if (id is int && name is String) {
          devices.add(
            GamepadDevice(
              id: id,
              name: name,
              hasRumble: entry['rumble'] == true,
            ),
          );
        }
      }
      return GamepadDevicesEvent(devices);
    }
    _log.warning('gamepad event of type ${raw.runtimeType} ignored');
    return null;
  }

  @override
  Future<bool> rumble(int deviceId, Duration duration, double amplitude) async {
    try {
      return await _platform.invokeMethod<bool>('gamepadRumble', {
            'deviceId': deviceId,
            'durationMs': duration.inMilliseconds,
            'amplitude': amplitude,
          }) ??
          false;
    } on PlatformException catch (error) {
      _log.warning('rumble: ${error.message}');
      return false;
    }
  }

  @override
  Future<bool> vibratePhone(Duration duration) async {
    try {
      return await _platform.invokeMethod<bool>('vibratePhone', {
            'durationMs': duration.inMilliseconds,
          }) ??
          false;
    } on PlatformException catch (error) {
      _log.warning('vibrate: ${error.message}');
      return false;
    }
  }
}
