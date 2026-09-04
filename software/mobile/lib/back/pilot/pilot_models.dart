import 'package:equatable/equatable.dart';
import 'package:mark4/gen/mark4.pbenum.dart';

/// Why the last arming gesture did nothing, shown until the next one.
enum ArmRefusal {
  none,

  /// The kill is latched: hold B to clear it first.
  killed,
  noGamepad,
  noDrone,

  /// The drone announces but streams no Status: no sensor pipeline.
  noStatus,

  /// The drone is not in IDLE (a cutoff, a fault, or already flying).
  notIdle,
  imuInvalid,
  baroInvalid,

  /// RT must be fully released to arm or to disarm (direct-thrust modes).
  throttleNotZero,

  /// The left stick must sit at its centre to arm or to disarm (altitude
  /// auto, where the centre means hold and anything else is a takeoff).
  throttleNotCentred,
  sticksNotCentered,
}

/// The button being held towards a gesture, for the progress ring.
enum PilotHold { none, arm, clearKill }

/// A haptic pattern, played on the controller or, failing that, the phone.
enum HapticCue {
  armed,
  disarmed,
  killed,
  killCleared,
  refused,
  modeChanged,
  linkLost,
  linkBack,
}

/// The piloting modes in the order the D-pad cycles them.
const List<RcMode> pilotModes = [
  RcMode.RC_MANUAL,
  RcMode.RC_LEVEL,
  RcMode.RC_ALTITUDE_AUTO,
];

/// The word for a mode, as the web console names it.
String pilotModeName(RcMode mode) => switch (mode) {
  RcMode.RC_MANUAL => 'manual',
  RcMode.RC_LEVEL => 'level',
  RcMode.RC_ALTITUDE_AUTO => 'altitude auto',
  _ => 'unknown',
};

/// The word for a phase, as the web console names it.
String flightPhaseName(FlightPhase phase) => switch (phase) {
  FlightPhase.PHASE_IDLE => 'idle',
  FlightPhase.PHASE_ALTITUDE_AUTO => 'altitude auto',
  FlightPhase.PHASE_ARMED => 'armed',
  FlightPhase.PHASE_BALLISTIC => 'ballistic',
  FlightPhase.PHASE_RECOVERY => 'recovery',
  FlightPhase.PHASE_HOVER => 'hover',
  FlightPhase.PHASE_CUTOFF => 'cutoff',
  FlightPhase.PHASE_MANUAL => 'manual',
  FlightPhase.PHASE_FAULT => 'fault',
  FlightPhase.PHASE_LEVEL => 'level',
  _ => 'unknown',
};

/// The four stick values of one Rc message, already mapped from the
/// controller: throttle in [0, 1], the rest in [-1, 1] in the pilot's
/// convention (right, nose down, clockwise positive).
class PilotSticks extends Equatable {
  const PilotSticks({
    this.throttle = 0,
    this.roll = 0,
    this.pitch = 0,
    this.yaw = 0,
  });

  static const PilotSticks released = PilotSticks();

  final double throttle;
  final double roll;
  final double pitch;
  final double yaw;

  @override
  List<Object?> get props => [throttle, roll, pitch, yaw];
}

/// What the pilot service knows: the latches, the mode, the sticks as
/// streamed, the three links, and the gesture in progress.
class PilotState extends Equatable {
  const PilotState({
    this.engaged = false,
    this.killed = true,
    this.armed = false,
    this.mode = RcMode.RC_MANUAL,
    this.sticks = PilotSticks.released,
    this.gamepadOk = false,
    this.droneOk = false,
    this.droneHearsUs = false,
    this.hold = PilotHold.none,
    this.holdProgress = 0,
    this.refusal = ArmRefusal.none,
    this.rcSent = 0,
  });

  static const PilotState idle = PilotState();

  /// The stream to the drone is running (the cockpit is open).
  final bool engaged;

  /// The kill is latched: every message says kill until B is held.
  final bool killed;

  /// The arm switch as streamed.
  final bool armed;

  /// The mode as streamed; the drone latches it on arming.
  final RcMode mode;

  /// The sticks as streamed.
  final PilotSticks sticks;

  /// A controller is present.
  final bool gamepadOk;

  /// The drone is heard.
  final bool droneOk;

  /// The drone reports hearing a pilot, and that report is fresh.
  final bool droneHearsUs;
  final PilotHold hold;

  /// Progress of the held gesture in [0, 1].
  final double holdProgress;
  final ArmRefusal refusal;

  /// Rc messages sent since boot.
  final int rcSent;

  /// Every link is up.
  bool get linksOk => gamepadOk && droneOk && droneHearsUs;

  PilotState copyWith({
    bool? engaged,
    bool? killed,
    bool? armed,
    RcMode? mode,
    PilotSticks? sticks,
    bool? gamepadOk,
    bool? droneOk,
    bool? droneHearsUs,
    PilotHold? hold,
    double? holdProgress,
    ArmRefusal? refusal,
    int? rcSent,
  }) => PilotState(
    engaged: engaged ?? this.engaged,
    killed: killed ?? this.killed,
    armed: armed ?? this.armed,
    mode: mode ?? this.mode,
    sticks: sticks ?? this.sticks,
    gamepadOk: gamepadOk ?? this.gamepadOk,
    droneOk: droneOk ?? this.droneOk,
    droneHearsUs: droneHearsUs ?? this.droneHearsUs,
    hold: hold ?? this.hold,
    holdProgress: holdProgress ?? this.holdProgress,
    refusal: refusal ?? this.refusal,
    rcSent: rcSent ?? this.rcSent,
  );

  @override
  List<Object?> get props => [
    engaged,
    killed,
    armed,
    mode,
    sticks,
    gamepadOk,
    droneOk,
    droneHearsUs,
    hold,
    holdProgress,
    refusal,
    rcSent,
  ];
}
