import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/pilot/pilot_manager.dart';
import 'package:mark4/back/pilot/pilot_models.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/back/transport/node_kind.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/gen/mark4.pbenum.dart';
import 'package:mark4/pages/drone/drone_bloc.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/widgets/info_row.dart';
import 'package:mark4/widgets/mark4_app_bar.dart';

/// One drone, flown: the cockpit. Connected and streaming while the page is
/// open; the hands are on the controller, the eyes glance here. Every
/// element is readable from a distance: the phase band first, then the
/// three links, then what the sticks and the motors do.
class DronePage extends StatefulWidget {
  const DronePage({required this.nodeId, super.key});

  final int nodeId;

  @override
  State<DronePage> createState() => _DronePageState();
}

class _DronePageState extends State<DronePage> {
  late final DroneBloc _bloc;
  late final AppLifecycleListener _lifecycle;

  @override
  void initState() {
    super.initState();
    _bloc = DroneBloc(
      drones: context.read<DroneManager>(),
      pilot: context.read<PilotManager>(),
      transport: context.read<TransportManager>(),
      nodeId: widget.nodeId,
    )..add(const DroneStarted());
    // A pilot who cannot see the drone is not piloting it.
    _lifecycle = AppLifecycleListener(
      onHide: () => _bloc.add(const DroneBackgrounded()),
    );
  }

  @override
  void dispose() {
    _lifecycle.dispose();
    _bloc.close();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return BlocProvider.value(value: _bloc, child: const _CockpitView());
  }
}

class _CockpitView extends StatelessWidget {
  const _CockpitView();

  @override
  Widget build(BuildContext context) {
    return BlocBuilder<DroneBloc, DroneState>(
      builder: (context, state) {
        final info = state.connection.info;
        final name = info?.announce.name ?? '';
        return Scaffold(
          appBar: Mark4AppBar(
            title: name.isEmpty ? formatNodeId(state.nodeId) : name,
          ),
          body: Column(
            children: [
              _PhaseBand(state: state),
              _Links(state: state),
              Expanded(
                child: ListView(
                  padding: EdgeInsets.all(AppSizes.gutter),
                  children: [
                    _ModeRow(state: state),
                    SizedBox(height: AppSizes.gap),
                    _Gesture(state: state),
                    SizedBox(height: AppSizes.gap),
                    _Throttle(state: state),
                    SizedBox(height: AppSizes.gap),
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        _Horizon(status: state.status),
                        SizedBox(width: AppSizes.gutter),
                        Expanded(child: _Motors(status: state.status)),
                      ],
                    ),
                    SizedBox(height: AppSizes.gap),
                    _Details(state: state),
                  ],
                ),
              ),
            ],
          ),
        );
      },
    );
  }
}

/// What the drone is doing, in one word and one color: the thing read from
/// the corner of the eye.
class _PhaseBand extends StatelessWidget {
  const _PhaseBand({required this.state});

  final DroneState state;

  @override
  Widget build(BuildContext context) {
    final colors = context.appColors;
    final text = Theme.of(context).textTheme;
    final (String word, Color color, IconData icon) = _look(colors);
    return Container(
      height: AppSizes.phaseBand,
      width: double.infinity,
      color: color,
      padding: EdgeInsets.symmetric(horizontal: AppSizes.gutter),
      child: Row(
        children: [
          Icon(icon, size: AppSizes.icon, color: colors.onStatus),
          SizedBox(width: AppSizes.gutter),
          Expanded(
            child: Text(
              word.toUpperCase(),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: text.headlineMedium?.copyWith(color: colors.onStatus),
            ),
          ),
        ],
      ),
    );
  }

  (String, Color, IconData) _look(AppColors colors) {
    if (!state.isConnected) {
      return (
        state.connection.info == null ? 'waiting for the drone' : 'drone lost',
        colors.alert,
        Icons.link_off,
      );
    }
    final status = state.status;
    if (status == null || !state.statusFresh) {
      return ('no status', colors.alert, Icons.sensors_off);
    }
    if (state.pilot.killed) {
      return ('killed', colors.alert, Icons.power_settings_new);
    }
    return switch (status.phase) {
      FlightPhase.PHASE_IDLE => ('idle', colors.idle, Icons.pause_circle),
      FlightPhase.PHASE_ARMED => ('armed', colors.warn, Icons.flight_takeoff),
      FlightPhase.PHASE_CUTOFF => ('cutoff', colors.alert, Icons.report),
      FlightPhase.PHASE_FAULT => ('fault', colors.alert, Icons.error),
      _ => (flightPhaseName(status.phase), colors.ok, Icons.flight),
    };
  }
}

/// The three links, each an icon in the link's color: the controller to the
/// phone, the phone to the drone (its beacons arrive), and the drone hearing
/// the phone (its Status says the RC fail-safe is not active).
class _Links extends StatelessWidget {
  const _Links({required this.state});

  final DroneState state;

  @override
  Widget build(BuildContext context) {
    final pilot = state.pilot;
    final heardAgo = state.connection.info?.lastSeenAgo;
    return Padding(
      padding: EdgeInsets.symmetric(
        horizontal: AppSizes.gutter,
        vertical: AppSizes.gapSmall,
      ),
      child: Row(
        children: [
          _LinkIndicator(
            ok: pilot.gamepadOk,
            icon: Icons.sports_esports,
            label: 'pad',
            detail: pilot.gamepadOk ? 'in hand' : 'none',
          ),
          _LinkIndicator(
            ok: state.isConnected,
            icon: Icons.wifi,
            label: 'drone',
            detail: heardAgo == null
                ? 'never'
                : '${heardAgo.inMilliseconds} ms',
          ),
          _LinkIndicator(
            ok: pilot.droneHearsUs,
            icon: Icons.hearing,
            label: 'hears us',
            detail: pilot.droneHearsUs
                ? '${pilot.rcSent} rc'
                : state.statusFresh
                ? 'silent'
                : 'no status',
          ),
        ],
      ),
    );
  }
}

class _LinkIndicator extends StatelessWidget {
  const _LinkIndicator({
    required this.ok,
    required this.icon,
    required this.label,
    required this.detail,
  });

  final bool ok;
  final IconData icon;
  final String label;
  final String detail;

  @override
  Widget build(BuildContext context) {
    final colors = context.appColors;
    final text = Theme.of(context).textTheme;
    final color = ok ? colors.ok : colors.alert;
    return Expanded(
      child: Row(
        children: [
          Icon(icon, size: AppSizes.icon, color: color),
          SizedBox(width: AppSizes.gapSmall),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(label, style: text.labelMedium),
                Text(
                  detail,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: text.labelMedium?.copyWith(color: colors.muted),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// The mode the phone streams: a choice, disabled while armed because the
/// drone locked its own on arming.
class _ModeRow extends StatelessWidget {
  const _ModeRow({required this.state});

  final DroneState state;

  @override
  Widget build(BuildContext context) {
    final pilot = state.pilot;
    final bloc = context.read<DroneBloc>();
    return SegmentedButton<RcMode>(
      segments: [
        for (final mode in pilotModes)
          ButtonSegment(value: mode, label: Text(pilotModeName(mode))),
      ],
      selected: {pilot.mode},
      showSelectedIcon: false,
      onSelectionChanged: pilot.armed
          ? null
          : (selection) => bloc.add(DroneModeSelected(selection.first)),
    );
  }
}

/// The gesture in progress (a ring filling while A or B is held), the
/// reason the last one was refused, or what to do next.
class _Gesture extends StatelessWidget {
  const _Gesture({required this.state});

  final DroneState state;

  @override
  Widget build(BuildContext context) {
    final pilot = state.pilot;
    final colors = context.appColors;
    final text = Theme.of(context).textTheme;
    final String hint;
    final Color color;
    if (pilot.hold != PilotHold.none) {
      hint = pilot.hold == PilotHold.arm
          ? (pilot.armed ? 'hold A to disarm' : 'hold A to arm')
          : 'hold B to clear the kill';
      color = colors.warn;
    } else if (pilot.refusal != ArmRefusal.none) {
      hint = _refusalText(pilot.refusal);
      color = colors.alert;
    } else if (pilot.killed) {
      hint = 'B held 1 s clears the kill';
      color = colors.muted;
    } else if (!pilot.armed) {
      hint = 'A held 1 s arms';
      color = colors.muted;
    } else if (pilot.mode == RcMode.RC_ALTITUDE_AUTO) {
      hint = 'armed: the left stick climbs and sinks, B kills';
      color = colors.ok;
    } else {
      hint = 'armed: RT is the throttle, B kills';
      color = colors.ok;
    }
    return Row(
      children: [
        SizedBox(
          width: AppSizes.icon,
          height: AppSizes.icon,
          child: CircularProgressIndicator(
            value: pilot.hold == PilotHold.none ? 0 : pilot.holdProgress,
            strokeWidth: AppSizes.stroke * 2,
            color: colors.warn,
            backgroundColor: Theme.of(
              context,
            ).colorScheme.surfaceContainerHighest,
          ),
        ),
        SizedBox(width: AppSizes.gutter),
        Expanded(
          child: Text(
            hint,
            maxLines: 2,
            style: text.titleMedium?.copyWith(color: color),
          ),
        ),
      ],
    );
  }

  static String _refusalText(ArmRefusal refusal) => switch (refusal) {
    ArmRefusal.none => '',
    ArmRefusal.killed => 'refused: clear the kill first (hold B)',
    ArmRefusal.noGamepad => 'refused: no gamepad',
    ArmRefusal.noDrone => 'refused: drone not heard',
    ArmRefusal.noStatus => 'refused: the drone streams no status',
    ArmRefusal.notIdle => 'refused: the drone is not idle',
    ArmRefusal.imuInvalid => 'refused: IMU invalid',
    ArmRefusal.baroInvalid => 'refused: baro invalid',
    ArmRefusal.throttleNotZero => 'refused: release RT',
    ArmRefusal.throttleNotCentred => 'refused: centre the left stick',
    ArmRefusal.sticksNotCentered => 'refused: centre the sticks',
  };
}

/// The throttle as streamed.
class _Throttle extends StatelessWidget {
  const _Throttle({required this.state});

  final DroneState state;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final text = Theme.of(context).textTheme;
    final throttle = state.pilot.sticks.throttle;
    return Row(
      children: [
        SizedBox(
          width: AppSizes.stickDot * 3,
          child: Text('throttle', style: text.labelLarge),
        ),
        Expanded(
          child: ClipRRect(
            borderRadius: BorderRadius.circular(AppSizes.radius / 2),
            child: LinearProgressIndicator(
              value: throttle.clamp(0.0, 1.0),
              minHeight: AppSizes.gauge,
              backgroundColor: scheme.surfaceContainerHighest,
              color: state.pilot.armed ? scheme.primary : scheme.outline,
            ),
          ),
        ),
        SizedBox(width: AppSizes.gapSmall),
        SizedBox(
          width: AppSizes.stickDot * 2,
          child: Text(
            '${(throttle * 100).round()}%',
            textAlign: TextAlign.end,
            style: text.bodyLarge,
          ),
        ),
      ],
    );
  }
}

/// A small artificial horizon from the estimated attitude.
class _Horizon extends StatelessWidget {
  const _Horizon({required this.status});

  final DroneStatus? status;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final side = AppSizes.horizon;
    return SizedBox(
      width: side,
      height: side,
      child: CustomPaint(
        painter: _HorizonPainter(
          rollRad: status?.rollRad ?? 0,
          pitchRad: status?.pitchRad ?? 0,
          sky: scheme.primaryContainer,
          ground: scheme.tertiaryContainer,
          frame: scheme.outline,
          mark: scheme.onSurface,
          stroke: AppSizes.stroke,
        ),
      ),
    );
  }
}

class _HorizonPainter extends CustomPainter {
  const _HorizonPainter({
    required this.rollRad,
    required this.pitchRad,
    required this.sky,
    required this.ground,
    required this.frame,
    required this.mark,
    required this.stroke,
  });

  final double rollRad;
  final double pitchRad;
  final Color sky;
  final Color ground;
  final Color frame;
  final Color mark;
  final double stroke;

  /// Pitch at which the horizon line reaches the edge of the square.
  static const double pitchFullScale = math.pi / 4;

  @override
  void paint(Canvas canvas, Size size) {
    final rect = Offset.zero & size;
    final center = rect.center;
    canvas.save();
    canvas.clipRect(rect);
    canvas.drawRect(rect, Paint()..color = sky);
    // Nose down moves the horizon up on the screen; a right roll tilts it
    // counter-clockwise, as seen from the pilot's seat.
    final shift =
        (pitchRad / pitchFullScale).clamp(-1.0, 1.0) * size.height / 2;
    canvas.translate(center.dx, center.dy - shift);
    canvas.rotate(-rollRad);
    final groundRect = Rect.fromLTWH(
      -size.width,
      0,
      size.width * 2,
      size.height * 2,
    );
    canvas.drawRect(groundRect, Paint()..color = ground);
    canvas.drawLine(
      Offset(-size.width, 0),
      Offset(size.width, 0),
      Paint()
        ..color = mark
        ..strokeWidth = stroke,
    );
    canvas.restore();
    // The fixed aircraft mark and the frame.
    final markPaint = Paint()
      ..color = mark
      ..strokeWidth = stroke * 1.5;
    canvas.drawLine(
      Offset(center.dx - size.width / 4, center.dy),
      Offset(center.dx - size.width / 10, center.dy),
      markPaint,
    );
    canvas.drawLine(
      Offset(center.dx + size.width / 10, center.dy),
      Offset(center.dx + size.width / 4, center.dy),
      markPaint,
    );
    canvas.drawCircle(center, stroke, markPaint);
    canvas.drawRect(
      rect,
      Paint()
        ..color = frame
        ..style = PaintingStyle.stroke
        ..strokeWidth = stroke,
    );
  }

  @override
  bool shouldRepaint(_HorizonPainter old) =>
      old.rollRad != rollRad || old.pitchRad != pitchRad || old.sky != sky;
}

/// The four motor commands as the drone reports them.
class _Motors extends StatelessWidget {
  const _Motors({required this.status});

  final DroneStatus? status;

  static const List<String> _names = ['RR', 'FR', 'RL', 'FL'];

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final text = Theme.of(context).textTheme;
    final motors = status?.motors ?? const [0.0, 0.0, 0.0, 0.0];
    return Column(
      children: [
        for (var i = 0; i < 4; ++i)
          Padding(
            padding: EdgeInsets.only(bottom: AppSizes.gapSmall),
            child: Row(
              children: [
                SizedBox(
                  width: AppSizes.stickDot * 1.5,
                  child: Text(_names[i], style: text.labelMedium),
                ),
                Expanded(
                  child: ClipRRect(
                    borderRadius: BorderRadius.circular(AppSizes.radius / 3),
                    child: LinearProgressIndicator(
                      value: motors[i].clamp(0.0, 1.0),
                      minHeight: AppSizes.motorBar,
                      backgroundColor: scheme.surfaceContainerHighest,
                      color: scheme.secondary,
                    ),
                  ),
                ),
              ],
            ),
          ),
      ],
    );
  }
}

/// What the drone announced and the link counters, folded: the old drone
/// page, for when something needs a closer look.
class _Details extends StatelessWidget {
  const _Details({required this.state});

  final DroneState state;

  @override
  Widget build(BuildContext context) {
    final info = state.connection.info;
    final status = state.status;
    return ExpansionTile(
      title: Text('Details', style: Theme.of(context).textTheme.titleMedium),
      tilePadding: EdgeInsets.zero,
      childrenPadding: EdgeInsets.zero,
      children: [
        InfoRow(label: 'Node', value: formatNodeId(state.nodeId), mono: true),
        if (status != null) ...[
          InfoRow(label: 'IMU', value: status.imuValid ? 'valid' : 'INVALID'),
          InfoRow(label: 'Baro', value: status.baroValid ? 'valid' : 'INVALID'),
          InfoRow(label: 'Throws', value: '${status.throwCount}'),
          InfoRow(
            label: 'Status age',
            value: '${state.statusAge?.inMilliseconds ?? 0} ms',
          ),
        ],
        if (info != null) ...[
          InfoRow(label: 'Kind', value: kindName(info.announce.kind)),
          InfoRow(label: 'MCU', value: info.announce.mcu.name),
          InfoRow(label: 'Address', value: info.address, mono: true),
          InfoRow(
            label: 'Build',
            value: info.announce.gitHash.isEmpty
                ? 'unknown'
                : info.announce.gitHash,
            mono: true,
          ),
          InfoRow(
            label: 'Wire',
            value: info.announce.wireMismatch ? 'MISMATCH' : 'same schema',
          ),
          InfoRow(
            label: 'Frames',
            value: '${info.received} received, ${info.lost} lost',
          ),
        ],
      ],
    );
  }
}
