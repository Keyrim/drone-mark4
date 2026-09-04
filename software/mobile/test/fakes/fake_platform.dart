import 'package:mark4/back/platform/abs_platform.dart';

/// A phone in a test: remembers what was asked.
class FakePlatform implements AbsPlatform {
  FakePlatform({this.name = 'Theo phone', this.lockGranted = true});

  final String name;
  final bool lockGranted;
  int lockAcquired = 0;
  int lockReleased = 0;

  @override
  Future<bool> acquireMulticastLock() async {
    ++lockAcquired;
    return lockGranted;
  }

  @override
  Future<void> releaseMulticastLock() async {
    ++lockReleased;
  }

  @override
  Future<String> deviceName() async => name;
}
