import 'dart:async';

import 'package:logging/logging.dart';
import 'package:mark4/back/gamepad/abs_gamepad_source.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';
import 'package:mark4/back/manager.dart';
import 'package:rxdart/rxdart.dart';

final Logger _log = Logger('back/gamepad');

/// The game controller: which ones the phone sees, every report they send,
/// and the haptics back to them. Reports go out raw on [samples] at the
/// controller's rate for whoever flies with them; [state] is the picture a
/// screen paints, refreshed at most every [statePeriod] so a 125 Hz
/// controller does not redraw a page 125 times a second.
class GamepadManager extends AbsManager {
  GamepadManager(
    this._source, {
    this.statePeriod = const Duration(milliseconds: 50),
    this.rateWindow = 32,
  });

  final AbsGamepadSource _source;

  /// Least time between two [state] values carrying a new sample.
  final Duration statePeriod;

  /// Reports the rate is averaged over.
  final int rateWindow;

  final BehaviorSubject<GamepadState> _state = BehaviorSubject.seeded(
    GamepadState.empty,
  );
  final PublishSubject<GamepadSample> _samples = PublishSubject();
  StreamSubscription<GamepadEvent>? _subscription;
  final List<double> _eventTimesMs = [];
  GamepadState _pending = GamepadState.empty;
  double _lastPublishedMs = double.negativeInfinity;
  Timer? _trailing;

  /// The controllers present, the last report, the report rate.
  ValueStream<GamepadState> get state => _state.stream;

  /// Every report, at the controller's rate.
  Stream<GamepadSample> get samples => _samples.stream;

  @override
  Future<bool> init() async {
    _subscription = _source.events.listen(
      _onEvent,
      onError: (Object error) => _log.warning('gamepad stream: $error'),
    );
    return true;
  }

  @override
  Future<void> dispose() async {
    _trailing?.cancel();
    _trailing = null;
    await _subscription?.cancel();
    _subscription = null;
    await _samples.close();
    await _state.close();
  }

  /// Rumbles the controller of the last report, or the first one present;
  /// false when none can.
  Future<bool> rumble({
    Duration duration = const Duration(milliseconds: 120),
    double amplitude = 1,
  }) async {
    final current = _state.value;
    final device = current.active ?? current.devices.firstOrNull;
    if (device == null || !device.hasRumble) {
      return false;
    }
    return _source.rumble(device.id, duration, amplitude);
  }

  /// Vibrates the phone; false when it cannot.
  Future<bool> vibratePhone({
    Duration duration = const Duration(milliseconds: 200),
  }) => _source.vibratePhone(duration);

  void _onEvent(GamepadEvent event) {
    switch (event) {
      case GamepadDevicesEvent(:final devices):
        _onDevices(devices);
      case GamepadSampleEvent(:final sample):
        _onSample(sample);
    }
  }

  void _onDevices(List<GamepadDevice> devices) {
    final before = _pending.devices;
    for (final device in devices) {
      if (!before.any((d) => d.id == device.id)) {
        _log.info(
          'gamepad "${device.name}" (${device.id}) connected'
          '${device.hasRumble ? ', rumble' : ''}',
        );
      }
    }
    for (final device in before) {
      if (!devices.any((d) => d.id == device.id)) {
        _log.info('gamepad "${device.name}" (${device.id}) gone');
      }
    }
    // A report from a controller that left is stale; drop it with the
    // controller so the picture never shows sticks nobody holds.
    final sampleGone =
        _pending.sample != null &&
        !devices.any((d) => d.id == _pending.sample!.deviceId);
    if (sampleGone) {
      _eventTimesMs.clear();
    }
    _pending = _pending.copyWith(
      devices: devices,
      clearSample: sampleGone,
      reportHz: sampleGone ? 0 : null,
    );
    _publish(force: true);
  }

  void _onSample(GamepadSample sample) {
    _samples.add(sample);
    _eventTimesMs.add(sample.eventTimeMs);
    if (_eventTimesMs.length > rateWindow) {
      _eventTimesMs.removeAt(0);
    }
    _pending = _pending.copyWith(
      sample: sample,
      sampleCount: _pending.sampleCount + 1,
      reportHz: _reportHz(),
    );
    _publish(force: false);
  }

  /// The mean rate over the window, 0 with fewer than two reports or when
  /// the timestamps do not move (a synthetic source).
  double _reportHz() {
    if (_eventTimesMs.length < 2) {
      return 0;
    }
    final spanMs = _eventTimesMs.last - _eventTimesMs.first;
    if (spanMs <= 0) {
      return 0;
    }
    return (_eventTimesMs.length - 1) * 1000 / spanMs;
  }

  /// Publishes the pending state: at once for a device change or the first
  /// report, otherwise no more often than [statePeriod] on the controller's
  /// clock. A report held back by the period is published by a trailing
  /// timer when no later one replaces it: a controller that stops sending
  /// once the sticks are released must still show them released.
  void _publish({required bool force}) {
    if (_pending == _state.value) {
      return;
    }
    final nowMs = _pending.sample?.eventTimeMs ?? _lastPublishedMs;
    final due =
        force ||
        _state.value.sample == null ||
        nowMs - _lastPublishedMs >= statePeriod.inMilliseconds;
    if (!due) {
      _trailing ??= Timer(statePeriod, () {
        _trailing = null;
        _publish(force: true);
      });
      return;
    }
    _trailing?.cancel();
    _trailing = null;
    _lastPublishedMs = nowMs;
    _state.add(_pending);
  }
}
