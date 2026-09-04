import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';
import 'package:mark4/back/pilot/pilot_manager.dart';
import 'package:mark4/back/pilot/pilot_models.dart';
import 'package:mark4/gen/mark4.pb.dart';

import '../fakes/bench.dart';

const int simId = 0x11111111;
const GamepadDevice xbox = GamepadDevice(id: 7, name: 'Xbox', hasRumble: true);

/// One controller report; buttons by bit.
GamepadSample pad({
  double leftX = 0,
  double leftY = 0,
  double rightX = 0,
  double rightY = 0,
  double rt = 0,
  List<int> down = const [],
}) {
  var buttons = GamepadButtons.none;
  for (final bit in down) {
    buttons = buttons.withBit(bit, down: true);
  }
  return GamepadSample(
    deviceId: xbox.id,
    eventTimeMs: 0,
    leftX: leftX,
    leftY: leftY,
    rightX: rightX,
    rightY: rightY,
    rightTrigger: rt,
    buttons: buttons,
  );
}

/// The drone's Status as it would broadcast it.
Envelope statusOf({
  FlightPhase phase = FlightPhase.PHASE_IDLE,
  bool imuValid = true,
  bool baroValid = true,
  bool rcLinkOk = true,
}) => Envelope()
  ..status = (Status()
    ..attitudeQuat.addAll([1, 0, 0, 0])
    ..motor.addAll([0, 0, 0, 0])
    ..flightPhase = phase
    ..imuValid = imuValid
    ..baroValid = baroValid
    ..rcLinkOk = rcLinkOk);

/// A cockpit in a test: drone announced and connected, controller present,
/// stream engaged.
class Cockpit {
  Cockpit(this.bench);

  final Bench bench;

  PilotManager get pilot => bench.backend.pilot;
  PilotState get state => pilot.state.value;

  /// The drone keeps broadcasting its last Status while time passes, as a
  /// live one does at 50 Hz; off to play a drone gone silent.
  bool droneStreams = true;
  Envelope _lastStatus = statusOf();

  /// The Rc messages sent to the drone so far, decoded.
  List<Rc> get sentRc {
    final rcs = <Rc>[];
    for (final (dst, payload) in bench.node.sent) {
      if (dst != simId) {
        continue;
      }
      final envelope = Envelope.fromBuffer(payload);
      if (envelope.hasRc()) {
        rcs.add(envelope.rc);
      }
    }
    return rcs;
  }

  Future<void> setUp() async {
    await bench.boot();
    bench.node.announceDrone(simId, 'drone_sim', NodeKind.DRONE_SIM, 1000);
    await bench.poll();
    await bench.backend.drones.connect(simId);
    bench.gamepad.devices([xbox]);
    await bench.settle();
    await pilot.engage();
    await status();
  }

  /// The drone broadcasts one Status; the phone hears it now.
  Future<void> status({
    FlightPhase phase = FlightPhase.PHASE_IDLE,
    bool imuValid = true,
    bool baroValid = true,
    bool rcLinkOk = true,
  }) async {
    _lastStatus = statusOf(
      phase: phase,
      imuValid: imuValid,
      baroValid: baroValid,
      rcLinkOk: rcLinkOk,
    );
    bench.node.receive(simId, _lastStatus);
    await bench.poll(advanceUs: 0);
  }

  /// One controller report, then one stream tick.
  Future<void> report(GamepadSample sample) async {
    bench.gamepad.report(sample);
    await bench.settle();
    pilot.tickNow();
    await bench.settle();
  }

  /// Time passes with the controller silent: [ms] of stream ticks, the
  /// drone repeating its Status every 100 ms while [droneStreams].
  Future<void> wait(int ms) async {
    for (var elapsed = 0; elapsed < ms; elapsed += 20) {
      bench.nowUs += 20000;
      if (droneStreams && elapsed % 100 == 0) {
        bench.node.receive(simId, _lastStatus);
        await bench.poll(advanceUs: 0);
      }
      pilot.tickNow();
    }
    await bench.settle();
  }

  /// Holds [down] for [ms] and releases.
  Future<void> hold(List<int> down, int ms, {double rt = 0}) async {
    await report(pad(down: down, rt: rt));
    await wait(ms);
    await report(pad(rt: rt));
  }

  /// The whole ritual: clear the kill (B held), then arm (A held); the
  /// drone follows into its rate mode, as a live one does within a frame.
  Future<void> clearKillAndArm() async {
    await hold([GamepadButtons.bitB], 1100);
    await status();
    await hold([GamepadButtons.bitA], 1100);
    await status(phase: FlightPhase.PHASE_MANUAL);
  }
}

void main() {
  late Bench bench;
  late Cockpit cockpit;

  setUp(() async {
    bench = Bench();
    cockpit = Cockpit(bench);
    await cockpit.setUp();
  });
  tearDown(() => bench.dispose());

  test(
    'an engaged cockpit streams the safe state until the kill is cleared',
    () async {
      expect(cockpit.state.engaged, isTrue);
      expect(cockpit.state.killed, isTrue);
      expect(cockpit.state.armed, isFalse);
      await cockpit.wait(100);
      final sent = cockpit.sentRc;
      expect(sent, isNotEmpty);
      expect(sent.every((rc) => rc.kill && !rc.arm), isTrue);
      expect(cockpit.state.linksOk, isTrue);
    },
  );

  test(
    'B held clears the kill, A held arms, RT then flies the throttle',
    () async {
      await cockpit.hold([GamepadButtons.bitB], 1100);
      expect(cockpit.state.killed, isFalse);
      expect(cockpit.state.armed, isFalse);

      await cockpit.status();
      await cockpit.hold([GamepadButtons.bitA], 1100);
      expect(cockpit.state.armed, isTrue);
      expect(cockpit.state.refusal, ArmRefusal.none);

      await cockpit.report(pad(rt: 0.6, rightX: 0.5, rightY: -0.25, leftX: -1));
      final rc = cockpit.sentRc.last;
      expect(rc.kill, isFalse);
      expect(rc.arm, isTrue);
      expect(rc.mode, RcMode.RC_MANUAL);
      expect(rc.throttle, closeTo(0.6, 1e-6));
      expect(rc.roll, closeTo(0.5, 1e-6));
      expect(rc.pitch, closeTo(0.25, 1e-6), reason: 'stick forward noses down');
      expect(rc.yaw, closeTo(-1, 1e-6));
    },
  );

  test(
    'a short A press arms nothing and the hold shows its progress',
    () async {
      await cockpit.hold([GamepadButtons.bitB], 1100);
      await cockpit.report(pad(down: [GamepadButtons.bitA]));
      await cockpit.wait(500);
      expect(cockpit.state.hold, PilotHold.arm);
      expect(cockpit.state.holdProgress, closeTo(0.5, 0.05));
      await cockpit.report(pad());
      expect(cockpit.state.armed, isFalse);
      expect(cockpit.state.hold, PilotHold.none);
    },
  );

  test(
    'arming is refused with the reason, and the killing press never clears',
    () async {
      // Killed: refused before anything else.
      await cockpit.hold([GamepadButtons.bitA], 1100);
      expect(cockpit.state.refusal, ArmRefusal.killed);

      await cockpit.hold([GamepadButtons.bitB], 1100);
      // RT not released.
      await cockpit.hold([GamepadButtons.bitA], 1100, rt: 0.3);
      expect(cockpit.state.armed, isFalse);
      expect(cockpit.state.refusal, ArmRefusal.throttleNotZero);

      // Drone not IDLE.
      await cockpit.status(phase: FlightPhase.PHASE_CUTOFF);
      await cockpit.hold([GamepadButtons.bitA], 1100);
      expect(cockpit.state.refusal, ArmRefusal.notIdle);

      // No IMU.
      await cockpit.status(imuValid: false);
      await cockpit.hold([GamepadButtons.bitA], 1100);
      expect(cockpit.state.refusal, ArmRefusal.imuInvalid);

      // Stale Status: the drone went silent.
      await cockpit.status();
      cockpit.droneStreams = false;
      await cockpit.wait(600);
      await cockpit.hold([GamepadButtons.bitA], 1100);
      expect(cockpit.state.refusal, ArmRefusal.noStatus);
      cockpit.droneStreams = true;

      // A B press while flying kills at once, and holding that same press
      // for over a second does not clear the kill it made.
      await cockpit.status();
      await cockpit.hold([GamepadButtons.bitA], 1100);
      expect(cockpit.state.armed, isTrue);
      await cockpit.report(pad(down: [GamepadButtons.bitB], rt: 0.5));
      expect(cockpit.state.killed, isTrue);
      expect(cockpit.state.armed, isFalse);
      expect(cockpit.sentRc.last.kill, isTrue);
      await cockpit.wait(1500);
      expect(cockpit.state.killed, isTrue);
      await cockpit.report(pad());
      expect(cockpit.state.killed, isTrue);
    },
  );

  test(
    'the D-pad cycles the mode left and right while disarmed only, and altitude auto moves the throttle to the left stick',
    () async {
      await cockpit.hold([GamepadButtons.bitB], 1100);
      expect(cockpit.state.mode, RcMode.RC_MANUAL);
      await cockpit.report(pad(down: [GamepadButtons.bitDpadRight]));
      await cockpit.report(pad());
      expect(cockpit.state.mode, RcMode.RC_LEVEL);
      await cockpit.report(pad(down: [GamepadButtons.bitDpadRight]));
      await cockpit.report(pad());
      expect(cockpit.state.mode, RcMode.RC_ALTITUDE_AUTO);

      // Left stick centred is mid throttle in that mode, RT ignored.
      await cockpit.report(pad(rt: 1));
      expect(cockpit.state.sticks.throttle, closeTo(0.5, 1e-6));
      await cockpit.report(pad(leftY: -1));
      expect(cockpit.state.sticks.throttle, closeTo(1, 1e-6));

      await cockpit.report(pad(down: [GamepadButtons.bitDpadLeft]));
      await cockpit.report(pad());
      await cockpit.report(pad(down: [GamepadButtons.bitDpadLeft]));
      await cockpit.report(pad());
      expect(cockpit.state.mode, RcMode.RC_MANUAL);

      // Armed: the D-pad does nothing.
      await cockpit.status();
      await cockpit.hold([GamepadButtons.bitA], 1100);
      expect(cockpit.state.armed, isTrue);
      await cockpit.report(pad(down: [GamepadButtons.bitDpadRight]));
      await cockpit.report(pad());
      expect(cockpit.state.mode, RcMode.RC_MANUAL);
      expect(cockpit.sentRc.last.mode, RcMode.RC_MANUAL);
    },
  );

  test('altitude auto arms and disarms with the left stick centred', () async {
    await cockpit.hold([GamepadButtons.bitB], 1100);
    await cockpit.report(pad(down: [GamepadButtons.bitDpadRight]));
    await cockpit.report(pad());
    await cockpit.report(pad(down: [GamepadButtons.bitDpadRight]));
    await cockpit.report(pad());
    expect(cockpit.state.mode, RcMode.RC_ALTITUDE_AUTO);

    // At rest the left stick is mid throttle: that is the interlock here,
    // RT plays no part.
    await cockpit.status();
    await cockpit.hold([GamepadButtons.bitA], 1100, rt: 0.8);
    expect(cockpit.state.armed, isTrue);
    expect(cockpit.sentRc.last.throttle, closeTo(0.5, 1e-6));
    await cockpit.status(phase: FlightPhase.PHASE_ARMED);

    // Pushed stick: no disarm.
    await cockpit.report(pad(leftY: -0.5, down: [GamepadButtons.bitA]));
    await cockpit.wait(1100);
    await cockpit.report(pad(leftY: -0.5));
    expect(cockpit.state.armed, isTrue);
    expect(cockpit.state.refusal, ArmRefusal.throttleNotCentred);

    // Centred: disarmed.
    await cockpit.hold([GamepadButtons.bitA], 1100);
    expect(cockpit.state.armed, isFalse);

    // And a pushed stick refuses to arm, naming the stick and not RT.
    await cockpit.status();
    await cockpit.report(pad(leftY: 0.4, down: [GamepadButtons.bitA]));
    await cockpit.wait(1100);
    await cockpit.report(pad(leftY: 0.4));
    expect(cockpit.state.armed, isFalse);
    expect(cockpit.state.refusal, ArmRefusal.throttleNotCentred);
  });

  test(
    'a simulated drone binds X and Y to its scene, a real one nothing',
    () async {
      expect(cockpit.state.actions, simActions);
      final before = bench.node.sent.length;
      await cockpit.report(pad(down: [GamepadButtons.bitX]));
      await cockpit.report(pad());
      await cockpit.report(pad(down: [GamepadButtons.bitY]));
      await cockpit.report(pad());
      final scenarios = <SimScenario>[];
      for (final (dst, payload) in bench.node.sent.sublist(before)) {
        final envelope = Envelope.fromBuffer(payload);
        if (dst == simId && envelope.hasSimScenario()) {
          scenarios.add(envelope.simScenario);
        }
      }
      expect(scenarios.map((s) => s.kind), [
        SimScenarioKind.RESET,
        SimScenarioKind.HAND_THROW,
      ]);
      expect(scenarios.map((s) => s.sequence), [1, 2]);
      expect(scenarios.last.heldSeconds, PilotManager.handThrowHeldS);
      expect(scenarios.last.velocityMps, [
        0,
        0,
        PilotManager.handThrowVelocityMps,
      ]);

      // The screen asks the same way.
      await cockpit.pilot.perform(PilotActionKind.resetScene);
      expect(bench.node.sent.length, greaterThan(before + 2));

      // A board offers nothing: X does nothing.
      const boardId = 0x22222222;
      bench.node.announceDrone(
        boardId,
        'mark4',
        NodeKind.FIRMWARE,
        bench.nowUs,
      );
      await bench.poll();
      await bench.backend.drones.connect(boardId);
      await bench.settle();
      expect(cockpit.state.actions, isEmpty);
      final sent = bench.node.sent.length;
      await cockpit.report(pad(down: [GamepadButtons.bitX]));
      await cockpit.report(pad());
      final toBoard = bench.node.sent
          .sublist(sent)
          .where((entry) => Envelope.fromBuffer(entry.$2).hasSimScenario());
      expect(toBoard, isEmpty);
    },
  );

  test('a scene action gives a restarting core time to re-arm', () async {
    await cockpit.clearKillAndArm();
    await cockpit.status(phase: FlightPhase.PHASE_ARMED);
    await cockpit.wait(400);
    expect(cockpit.state.armed, isTrue);

    // The hand throw resets the plant: the core restarts and reports IDLE
    // for a while before it re-arms on the switch it still sees.
    await cockpit.report(pad(down: [GamepadButtons.bitY]));
    await cockpit.report(pad());
    await cockpit.status();
    await cockpit.wait(1500);
    expect(cockpit.state.armed, isTrue, reason: 'grace after the scenario');
    await cockpit.status(phase: FlightPhase.PHASE_ARMED);
    await cockpit.wait(1000);
    expect(cockpit.state.armed, isTrue);

    // A core that never re-arms does disarm the phone once the grace is over.
    await cockpit.report(pad(down: [GamepadButtons.bitX]));
    await cockpit.report(pad());
    await cockpit.status();
    await cockpit.wait(2500);
    expect(cockpit.state.armed, isFalse);
  });

  test('disarming wants RT released', () async {
    await cockpit.clearKillAndArm();
    expect(cockpit.state.armed, isTrue);
    await cockpit.hold([GamepadButtons.bitA], 1100, rt: 0.4);
    expect(cockpit.state.armed, isTrue);
    expect(cockpit.state.refusal, ArmRefusal.throttleNotZero);
    await cockpit.hold([GamepadButtons.bitA], 1100);
    expect(cockpit.state.armed, isFalse);
  });

  test('the controller leaving kills at once', () async {
    await cockpit.clearKillAndArm();
    await cockpit.report(pad(rt: 0.5));
    bench.gamepad.devices([]);
    await bench.settle();
    expect(cockpit.state.killed, isTrue);
    expect(cockpit.state.armed, isFalse);
    expect(cockpit.state.gamepadOk, isFalse);
    expect(cockpit.state.sticks, PilotSticks.released);
    final last = cockpit.sentRc.last;
    expect(last.kill, isTrue);
    expect(last.throttle, 0);
  });

  test(
    'a drone that dropped out of the armed phases disarms the phone',
    () async {
      await cockpit.clearKillAndArm();
      expect(cockpit.state.armed, isTrue);
      // Right after arming the drone is still IDLE for a few frames: grace.
      await cockpit.status();
      await cockpit.wait(100);
      expect(cockpit.state.armed, isTrue);
      await cockpit.status(phase: FlightPhase.PHASE_MANUAL);
      await cockpit.wait(400);
      expect(cockpit.state.armed, isTrue);
      // The drone's fail-safe tripped: back to IDLE while we still say armed.
      await cockpit.status();
      await cockpit.wait(40);
      expect(cockpit.state.armed, isFalse);
      expect(cockpit.state.killed, isFalse, reason: 'a disarm, not a kill');
    },
  );

  test('the drone hearing us is the Status flag, fresh', () async {
    expect(cockpit.state.droneHearsUs, isTrue);
    await cockpit.status(rcLinkOk: false);
    await cockpit.wait(20);
    expect(cockpit.state.droneHearsUs, isFalse);
    await cockpit.status();
    await cockpit.wait(20);
    expect(cockpit.state.droneHearsUs, isTrue);
    cockpit.droneStreams = false;
    await cockpit.wait(600);
    expect(cockpit.state.droneHearsUs, isFalse, reason: 'stale');
  });

  test('losing the drone marks the link and the haptics say so', () async {
    bench.gamepad.rumbles.clear();
    bench.node.forget(simId);
    await bench.poll();
    await cockpit.wait(40);
    expect(cockpit.state.droneOk, isFalse);
    expect(bench.gamepad.rumbles, isNotEmpty);
  });

  test('disengaging sends the safe state twice and stops', () async {
    await cockpit.clearKillAndArm();
    final before = cockpit.sentRc.length;
    await cockpit.pilot.disengage();
    final sent = cockpit.sentRc;
    expect(sent.length, before + PilotManager.goodbyeFrames);
    expect(sent.last.kill, isTrue);
    expect(sent.last.arm, isFalse);
    expect(cockpit.state.engaged, isFalse);
    cockpit.pilot.tickNow();
    expect(cockpit.sentRc.length, sent.length);

    // Engaging again starts killed: the ritual is owed again.
    await cockpit.pilot.engage();
    expect(cockpit.state.killed, isTrue);
    expect(cockpit.state.armed, isFalse);
  });

  test('killNow latches and sends', () async {
    await cockpit.clearKillAndArm();
    await cockpit.pilot.killNow();
    expect(cockpit.state.killed, isTrue);
    expect(cockpit.sentRc.last.kill, isTrue);
  });

  test('the haptics reach the controller', () async {
    bench.gamepad.rumbles.clear();
    await cockpit.hold([GamepadButtons.bitB], 1100);
    await Future<void>.delayed(const Duration(milliseconds: 100));
    expect(bench.gamepad.rumbles, isNotEmpty);
    expect(bench.gamepad.rumbles.first.$1, xbox.id);
  });
}
