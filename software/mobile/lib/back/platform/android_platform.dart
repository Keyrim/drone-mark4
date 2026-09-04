import 'package:flutter/services.dart';
import 'package:logging/logging.dart';
import 'package:mark4/back/platform/abs_platform.dart';

final Logger _log = Logger('back/platform');

/// The Android side: one method channel answered by MainActivity.kt.
class AndroidPlatform implements AbsPlatform {
  static const MethodChannel _channel = MethodChannel('mark4/platform');

  @override
  Future<bool> acquireMulticastLock() async {
    try {
      return await _channel.invokeMethod<bool>('acquireMulticastLock') ?? false;
    } on PlatformException catch (error) {
      _log.warning('multicast lock: ${error.message}');
      return false;
    }
  }

  @override
  Future<void> releaseMulticastLock() async {
    try {
      await _channel.invokeMethod<void>('releaseMulticastLock');
    } on PlatformException catch (error) {
      _log.warning('multicast lock release: ${error.message}');
    }
  }

  @override
  Future<String> deviceName() async {
    try {
      return await _channel.invokeMethod<String>('deviceName') ?? 'phone';
    } on PlatformException catch (error) {
      _log.warning('device name: ${error.message}');
      return 'phone';
    }
  }
}
