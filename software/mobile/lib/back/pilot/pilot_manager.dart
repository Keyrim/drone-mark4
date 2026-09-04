import 'dart:async';

import 'package:logging/logging.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/gamepad/gamepad_manager.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';
import 'package:mark4/back/manager.dart';
import 'package:mark4/back/pilot/pilot_models.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/gen/mark4.pb.dart';
import 'package:rxdart/rxdart.dart';

final Logger _log = Logger('back/pilot');

/// The transmitter: turns the controller into the Rc stream the connected
/// drone flies on, and holds the two latches no stick may flip.
///
/// The gestures (Xbox layout, agreed in docs/mobile-app.md):
/// - B kills at once, one press, and the kill stays latched; holding B for
///   [holdDuration] while killed clears it. Every session starts killed.
/// - A held for [holdDuration] arms, if the drone is IDLE with valid
///   sensors, RT is released and the sticks are centred; held again with RT
///   released it disarms. A refused gesture says why.
/// - D-pad up / down cycles the mode, while disarmed only.
/// - RT is the throttle in the direct-thrust modes (released = motors
///   stopped); the left stick's vertical axis is the throttle of altitude
///   auto, where centre means hold. Right stick roll and pitch, left stick
///   yaw.
///
/// The stream runs at [period] from [engage] to [disengage]; a controller
/// leaving kills at once, leaving the cockpit sends the safe state and
/// stops, and the drone's own fail-safe covers everything else: silence is
/// a kill. Haptic cues go to the controller, or the phone when it cannot.
class PilotManager extends AbsManager {
  PilotManager({
    required this._gamepad,
    required this._drones,
    required this._transport,
    int Function()? clockUs,
    this.period = const Duration(milliseconds: 20),
    this.holdDuration = const Duration(seconds: 1),
    this.statusStaleAfter = const Duration(milliseconds: 500),
    this.armSettle = const Duration(milliseconds: 300),
    this.linkAlarmPeriod = const Duration(seconds: 2),
    this.statePeriod = const Duration(milliseconds: 50),
  }) : _clockUs = clockUs ?? _stopwatchClock();

  /// RT under this counts as released.
  static const double throttleZero = 0.02;

  /// A stick within this of the centre counts as centred.
  static const double stickCentre = 0.15;

  /// How many times the safe state goes out when the stream stops.
  static const int goodbyeFrames = 2;

  final GamepadManager _gamepad;
  final DroneManager _drones;
  final TransportManager _transport;
  final int Function() _clockUs;

  /// Rc stream period; null when the owner calls [tickNow] itself (tests).
  final Duration? period;

  /// How long A or B is held for a gesture.
  final Duration holdDuration;

  /// A Status older than this says nothing about the drone any more.
  final Duration statusStaleAfter;

  /// Grace after arming before a drone still IDLE means it refused.
  final Duration armSettle;

  /// Cadence of the repeated alarm while a link is lost.
  final Duration linkAlarmPeriod;

  /// Least time between two state values differing only by the sticks.
  final Duration statePeriod;

  final BehaviorSubject<PilotState> _state = BehaviorSubject.seeded(
    PilotState.idle,
  );
  final List<StreamSubscription<Object?>> _subscriptions = [];
  Timer? _timer;
  PilotState _pending = PilotState.idle;
  int _lastPublishedUs = 0;
  GamepadButtons _buttons = GamepadButtons.none;
  int? _armHoldSinceUs;
  bool _armHoldDone = false;
  int? _killHoldSinceUs;
  int _armedAtUs = 0;
  int _lastLinkAlarmUs = 0;
  bool _linksWereOk = true;

  /// The transmitter as it is.
  ValueStream<PilotState> get state => _state.stream;

  @override
  Future<bool> init() async {
    _subscriptions.add(_gamepad.samples.listen(_onSample));
    _subscriptions.add(_gamepad.state.listen(_onGamepad));
    _subscriptions.add(_drones.connection.listen((_) => _refreshLinks()));
    _subscriptions.add(_drones.status.listen((_) => _refreshLinks()));
    return true;
  }

  @override
  Future<void> dispose() async {
    _timer?.cancel();
    _timer = null;
    for (final subscription in _subscriptions) {
      await subscription.cancel();
    }
    _subscriptions.clear();
    await _state.close();
  }

  /// Starts streaming to the connected drone, from the safe state: killed,
  /// disarmed, sticks released. The pilot clears the kill by hand.
  Future<void> engage() async {
    if (_pending.engaged) {
      return;
    }
    _log.info('engaged, killed until B is held');
    _pending = _pending.copyWith(
      engaged: true,
      killed: true,
      armed: false,
      hold: PilotHold.none,
      holdProgress: 0,
      refusal: ArmRefusal.none,
    );
    _armHoldSinceUs = null;
    _killHoldSinceUs = null;
    _linksWereOk = true;
    _lastLinkAlarmUs = 0;
    _publish(force: true);
    final streamPeriod = period;
    if (streamPeriod != null) {
      _timer = Timer.periodic(streamPeriod, (_) => tickNow());
    }
    tickNow();
  }

  /// Sends the safe state [goodbyeFrames] times and stops the stream.
  Future<void> disengage() async {
    if (!_pending.engaged) {
      return;
    }
    _timer?.cancel();
    _timer = null;
    _pending = _pending.copyWith(
      killed: true,
      armed: false,
      sticks: PilotSticks.released,
      hold: PilotHold.none,
      holdProgress: 0,
    );
    for (var i = 0; i < goodbyeFrames; ++i) {
      _send();
    }
    _pending = _pending.copyWith(engaged: false);
    _log.info('disengaged, safe state sent');
    _publish(force: true);
  }

  /// Latches the kill now and sends it: what a pilot who cannot see the
  /// drone (the app went to the background) asks for.
  Future<void> killNow() async {
    if (!_pending.killed) {
      _kill('kill requested');
      _send();
      _publish(force: true);
    }
  }

  /// Selects the mode, disarmed only (the drone latches it on arming).
  Future<void> selectMode(RcMode mode) async {
    if (_pending.armed || mode == _pending.mode) {
      return;
    }
    _pending = _pending.copyWith(mode: mode);
    _log.info('mode ${pilotModeName(mode)}');
    _cue(HapticCue.modeChanged);
    _publish(force: true);
  }

  /// One step of the stream: the held gestures, the links, one Rc message.
  /// What the timer does every [period].
  void tickNow() {
    if (!_pending.engaged) {
      return;
    }
    final nowUs = _clockUs();
    _advanceHolds(nowUs);
    _watchDrone(nowUs);
    _alarmLinks(nowUs);
    _send();
    _publish(force: false);
  }

  // ---- controller

  void _onSample(GamepadSample sample) {
    final before = _buttons;
    final after = sample.buttons;
    _buttons = after;
    _pending = _pending.copyWith(sticks: _mapSticks(sample, _pending.mode));
    if (!_pending.engaged) {
      _publish(force: false);
      return;
    }
    final nowUs = _clockUs();
    // B: the kill, at once on the press; a hold clears it only when it was
    // already latched before the press, so the killing press never clears.
    if (after.b && !before.b) {
      if (_pending.killed) {
        _killHoldSinceUs = nowUs;
      } else {
        _kill('B pressed');
        _send();
      }
    } else if (!after.b && before.b) {
      _killHoldSinceUs = null;
    }
    // A: a hold towards arming or disarming.
    if (after.a && !before.a) {
      _armHoldSinceUs = nowUs;
      _armHoldDone = false;
      _pending = _pending.copyWith(refusal: ArmRefusal.none);
    } else if (!after.a && before.a) {
      _armHoldSinceUs = null;
      _armHoldDone = false;
    }
    // D-pad: the mode, disarmed only.
    if (after.dpadUp && !before.dpadUp) {
      _cycleMode(1);
    } else if (after.dpadDown && !before.dpadDown) {
      _cycleMode(-1);
    }
    _refreshHold(nowUs);
    _publish(force: false);
  }

  void _onGamepad(GamepadState gamepad) {
    final wasOk = _pending.gamepadOk;
    _pending = _pending.copyWith(gamepadOk: gamepad.connected);
    if (wasOk && !gamepad.connected) {
      _buttons = GamepadButtons.none;
      _armHoldSinceUs = null;
      _killHoldSinceUs = null;
      _pending = _pending.copyWith(sticks: PilotSticks.released);
      if (_pending.engaged) {
        // The hands left the controls: cut now rather than letting the
        // last sticks fly for the drone's whole fail-safe window.
        _kill('gamepad lost');
        _send();
      }
    }
    _refreshLinks();
  }

  /// Android's convention (right and down positive, triggers in [0, 1])
  /// onto the pilot's: pitch flips so pushing the stick forward noses
  /// down, RT is the collective of the direct-thrust modes, and the left
  /// stick's vertical axis is the velocity stick of altitude auto, its
  /// centre the hold.
  static PilotSticks _mapSticks(GamepadSample sample, RcMode mode) {
    final throttle = mode == RcMode.RC_ALTITUDE_AUTO
        ? (1 - sample.leftY) / 2
        : sample.rightTrigger;
    return PilotSticks(
      throttle: throttle.clamp(0.0, 1.0),
      roll: sample.rightX.clamp(-1.0, 1.0),
      pitch: (-sample.rightY).clamp(-1.0, 1.0),
      yaw: sample.leftX.clamp(-1.0, 1.0),
    );
  }

  void _cycleMode(int step) {
    if (_pending.armed) {
      return;
    }
    final index = pilotModes.indexOf(_pending.mode);
    final next = pilotModes[(index + step) % pilotModes.length];
    unawaited(selectMode(next));
  }

  // ---- gestures

  void _advanceHolds(int nowUs) {
    final armSince = _armHoldSinceUs;
    if (armSince != null &&
        !_armHoldDone &&
        nowUs - armSince >= holdDuration.inMicroseconds) {
      _armHoldDone = true;
      if (_pending.armed) {
        _disarmGesture();
      } else {
        _armGesture(nowUs);
      }
    }
    final killSince = _killHoldSinceUs;
    if (killSince != null &&
        _pending.killed &&
        nowUs - killSince >= holdDuration.inMicroseconds) {
      _killHoldSinceUs = null;
      _pending = _pending.copyWith(killed: false, refusal: ArmRefusal.none);
      _log.info('kill cleared');
      _cue(HapticCue.killCleared);
    }
    _refreshHold(nowUs);
  }

  void _refreshHold(int nowUs) {
    final armSince = _armHoldSinceUs;
    final killSince = _killHoldSinceUs;
    if (armSince != null && !_armHoldDone) {
      _pending = _pending.copyWith(
        hold: PilotHold.arm,
        holdProgress: _progress(nowUs - armSince),
      );
    } else if (killSince != null && _pending.killed) {
      _pending = _pending.copyWith(
        hold: PilotHold.clearKill,
        holdProgress: _progress(nowUs - killSince),
      );
    } else {
      _pending = _pending.copyWith(hold: PilotHold.none, holdProgress: 0);
    }
  }

  double _progress(int heldUs) =>
      (heldUs / holdDuration.inMicroseconds).clamp(0.0, 1.0);

  void _armGesture(int nowUs) {
    final refusal = _armRefusal(nowUs);
    if (refusal != ArmRefusal.none) {
      _pending = _pending.copyWith(refusal: refusal);
      _log.info('arm refused: ${refusal.name}');
      _cue(HapticCue.refused);
      return;
    }
    _armedAtUs = nowUs;
    _pending = _pending.copyWith(armed: true, refusal: ArmRefusal.none);
    _log.info('armed, mode ${pilotModeName(_pending.mode)}');
    _cue(HapticCue.armed);
  }

  void _disarmGesture() {
    if (_pending.sticks.throttle > throttleZero) {
      _pending = _pending.copyWith(refusal: ArmRefusal.throttleNotZero);
      _log.info('disarm refused: throttle not zero');
      _cue(HapticCue.refused);
      return;
    }
    _pending = _pending.copyWith(armed: false);
    _log.info('disarmed');
    _cue(HapticCue.disarmed);
  }

  /// The conditions of arming, checked here so the screen can say which
  /// one is missing; the drone checks its own again.
  ArmRefusal _armRefusal(int nowUs) {
    if (_pending.killed) {
      return ArmRefusal.killed;
    }
    if (!_pending.gamepadOk) {
      return ArmRefusal.noGamepad;
    }
    if (!_pending.droneOk) {
      return ArmRefusal.noDrone;
    }
    final status = _drones.status.value;
    if (status == null || _stale(status, nowUs)) {
      return ArmRefusal.noStatus;
    }
    if (status.phase != FlightPhase.PHASE_IDLE) {
      return ArmRefusal.notIdle;
    }
    if (!status.imuValid) {
      return ArmRefusal.imuInvalid;
    }
    if (!status.baroValid) {
      return ArmRefusal.baroInvalid;
    }
    final sticks = _pending.sticks;
    if (sticks.throttle > throttleZero) {
      return ArmRefusal.throttleNotZero;
    }
    if (sticks.roll.abs() > stickCentre ||
        sticks.pitch.abs() > stickCentre ||
        sticks.yaw.abs() > stickCentre) {
      return ArmRefusal.sticksNotCentered;
    }
    return ArmRefusal.none;
  }

  void _kill(String why) {
    _pending = _pending.copyWith(
      killed: true,
      armed: false,
      refusal: ArmRefusal.none,
    );
    _armHoldSinceUs = null;
    _armHoldDone = false;
    _log.warning('kill: $why');
    _cue(HapticCue.killed);
  }

  // ---- drone and links

  bool _stale(DroneStatus status, int nowUs) =>
      nowUs - status.receivedAtUs > statusStaleAfter.inMicroseconds;

  void _refreshLinks() {
    final nowUs = _clockUs();
    final connection = _drones.connection.value;
    final status = _drones.status.value;
    final droneOk = connection.status == DroneLinkStatus.connected;
    final hearsUs = status != null && !_stale(status, nowUs) && status.rcLinkOk;
    _pending = _pending.copyWith(droneOk: droneOk, droneHearsUs: hearsUs);
    _publish(force: false);
  }

  /// A drone that left the armed phases while the phone still says armed
  /// (its fail-safe tripped, a cutoff, a fault) disarms the phone too, so
  /// flying again is a deliberate gesture and never a stream resuming.
  void _watchDrone(int nowUs) {
    if (!_pending.armed || nowUs - _armedAtUs < armSettle.inMicroseconds) {
      return;
    }
    final status = _drones.status.value;
    if (status != null && !_stale(status, nowUs) && !status.armed) {
      _pending = _pending.copyWith(armed: false);
      _log.warning('drone reports ${flightPhaseName(status.phase)}: disarmed');
      _cue(HapticCue.disarmed);
    }
  }

  void _alarmLinks(int nowUs) {
    _refreshLinks();
    final ok = _pending.gamepadOk && _pending.droneOk;
    if (ok != _linksWereOk) {
      _linksWereOk = ok;
      _lastLinkAlarmUs = nowUs;
      _cue(ok ? HapticCue.linkBack : HapticCue.linkLost);
    } else if (!ok &&
        nowUs - _lastLinkAlarmUs >= linkAlarmPeriod.inMicroseconds) {
      _lastLinkAlarmUs = nowUs;
      _cue(HapticCue.linkLost);
    }
  }

  // ---- output

  void _send() {
    final connection = _drones.connection.value;
    if (connection.status == DroneLinkStatus.none) {
      return;
    }
    final sticks = _pending.sticks;
    final envelope = Envelope()
      ..rc = (Rc()
        ..kill = _pending.killed
        ..arm = _pending.armed && !_pending.killed
        ..mode = _pending.mode
        ..throttle = sticks.throttle
        ..roll = sticks.roll
        ..pitch = sticks.pitch
        ..yaw = sticks.yaw);
    unawaited(_transport.send(connection.nodeId, envelope));
    _pending = _pending.copyWith(rcSent: _pending.rcSent + 1);
  }

  /// Publishes the pending state: at once when anything but the sticks or
  /// the counter changed, else at most every [statePeriod].
  void _publish({required bool force}) {
    final current = _state.value;
    if (_pending == current) {
      return;
    }
    final nowUs = _clockUs();
    final discrete =
        _pending.copyWith(sticks: current.sticks, rcSent: current.rcSent) !=
        current;
    if (!force &&
        !discrete &&
        nowUs - _lastPublishedUs < statePeriod.inMicroseconds) {
      return;
    }
    _lastPublishedUs = nowUs;
    _state.add(_pending);
  }

  void _cue(HapticCue cue) {
    unawaited(_play(cue));
  }

  /// Plays one cue on the controller, or the phone when it has no rumble.
  Future<void> _play(HapticCue cue) async {
    final pattern = switch (cue) {
      HapticCue.armed => const [80, 120, 80],
      HapticCue.disarmed => const [80],
      HapticCue.killed => const [400],
      HapticCue.killCleared => const [60],
      HapticCue.refused => const [40, 80, 40],
      HapticCue.modeChanged => const [30],
      HapticCue.linkLost => const [60, 80, 60, 80, 60],
      HapticCue.linkBack => const [80],
    };
    for (var i = 0; i < pattern.length; ++i) {
      final ms = pattern[i];
      if (i.isEven) {
        final duration = Duration(milliseconds: ms);
        if (!await _gamepad.rumble(duration: duration)) {
          await _gamepad.vibratePhone(duration: duration);
        }
      }
      await Future<void>.delayed(Duration(milliseconds: ms));
    }
  }

  static int Function() _stopwatchClock() {
    final stopwatch = Stopwatch()..start();
    return () => stopwatch.elapsedMicroseconds;
  }
}
