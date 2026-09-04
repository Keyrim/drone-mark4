import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/gamepad/gamepad_manager.dart';
import 'package:mark4/back/gamepad/gamepad_models.dart';
import 'package:mark4/pages/gamepad/gamepad_bloc.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/widgets/info_row.dart';
import 'package:mark4/widgets/mark4_app_bar.dart';
import 'package:mark4/widgets/status_banner.dart';

/// The controller in hand: is it there, what does it report, does it buzz.
/// The page to open when a stick feels wrong, before blaming the drone.
class GamepadPage extends StatelessWidget {
  const GamepadPage({super.key});

  @override
  Widget build(BuildContext context) {
    return BlocProvider(
      create: (context) =>
          GamepadBloc(context.read<GamepadManager>())
            ..add(const GamepadPageStarted()),
      child: const _GamepadView(),
    );
  }
}

class _GamepadView extends StatelessWidget {
  const _GamepadView();

  @override
  Widget build(BuildContext context) {
    return BlocBuilder<GamepadBloc, GamepadPageState>(
      builder: (context, state) {
        final gamepad = state.gamepad;
        final device = gamepad.active ?? gamepad.devices.firstOrNull;
        final sample = gamepad.sample;
        return Scaffold(
          appBar: const Mark4AppBar(title: 'Gamepad'),
          body: Column(
            children: [
              StatusBanner(
                ok: gamepad.connected,
                text: device == null
                    ? 'No gamepad: pair one in the Bluetooth settings'
                    : device.name,
              ),
              Expanded(
                child: ListView(
                  padding: EdgeInsets.all(AppSizes.gutter),
                  children: [
                    if (device != null) ...[
                      InfoRow(label: 'Device id', value: '${device.id}'),
                      InfoRow(
                        label: 'Rumble',
                        value: device.hasRumble ? 'yes' : 'no',
                      ),
                    ],
                    InfoRow(
                      label: 'Reports',
                      value: gamepad.reportHz == 0
                          ? '${gamepad.sampleCount}'
                          : '${gamepad.sampleCount} at '
                                '${gamepad.reportHz.toStringAsFixed(0)} Hz',
                    ),
                    SizedBox(height: AppSizes.gap),
                    _Sticks(sample: sample),
                    SizedBox(height: AppSizes.gap),
                    _Trigger(label: 'LT', value: sample?.leftTrigger ?? 0),
                    SizedBox(height: AppSizes.gapSmall),
                    _Trigger(label: 'RT', value: sample?.rightTrigger ?? 0),
                    SizedBox(height: AppSizes.gap),
                    _Buttons(buttons: sample?.buttons ?? GamepadButtons.none),
                    SizedBox(height: AppSizes.gap),
                    _Haptics(state: state),
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

/// The two sticks, each a square with a dot where the stick is.
class _Sticks extends StatelessWidget {
  const _Sticks({required this.sample});

  final GamepadSample? sample;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceEvenly,
      children: [
        _StickPad(
          label: 'L',
          x: sample?.leftX ?? 0,
          y: sample?.leftY ?? 0,
          pressed: sample?.buttons.leftThumb ?? false,
        ),
        _StickPad(
          label: 'R',
          x: sample?.rightX ?? 0,
          y: sample?.rightY ?? 0,
          pressed: sample?.buttons.rightThumb ?? false,
        ),
      ],
    );
  }
}

class _StickPad extends StatelessWidget {
  const _StickPad({
    required this.label,
    required this.x,
    required this.y,
    required this.pressed,
  });

  final String label;
  final double x;
  final double y;
  final bool pressed;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final side = AppSizes.stickPad;
    return Column(
      children: [
        Text(label, style: Theme.of(context).textTheme.labelLarge),
        SizedBox(height: AppSizes.gapSmall),
        SizedBox(
          width: side,
          height: side,
          child: CustomPaint(
            painter: _StickPainter(
              x: x,
              y: y,
              frame: scheme.outline,
              dot: pressed ? scheme.primary : scheme.onSurface,
              dotDiameter: AppSizes.stickDot,
              stroke: AppSizes.stroke,
            ),
          ),
        ),
        SizedBox(height: AppSizes.gapSmall),
        Text(
          '${x.toStringAsFixed(2)}  ${y.toStringAsFixed(2)}',
          style: Theme.of(
            context,
          ).textTheme.bodyMedium?.copyWith(color: context.appColors.muted),
        ),
      ],
    );
  }
}

class _StickPainter extends CustomPainter {
  const _StickPainter({
    required this.x,
    required this.y,
    required this.frame,
    required this.dot,
    required this.dotDiameter,
    required this.stroke,
  });

  final double x;
  final double y;
  final Color frame;
  final Color dot;
  final double dotDiameter;
  final double stroke;

  @override
  void paint(Canvas canvas, Size size) {
    final framePaint = Paint()
      ..color = frame
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke;
    final rect = Offset.zero & size;
    canvas.drawRect(rect, framePaint);
    canvas.drawLine(
      Offset(size.width / 2, 0),
      Offset(size.width / 2, size.height),
      framePaint,
    );
    canvas.drawLine(
      Offset(0, size.height / 2),
      Offset(size.width, size.height / 2),
      framePaint,
    );
    // Android's stick convention is right and down positive, which is the
    // screen's: no flip.
    final radius = dotDiameter / 2;
    final center = Offset(
      radius + (x.clamp(-1.0, 1.0) + 1) / 2 * (size.width - dotDiameter),
      radius + (y.clamp(-1.0, 1.0) + 1) / 2 * (size.height - dotDiameter),
    );
    canvas.drawCircle(center, radius, Paint()..color = dot);
  }

  @override
  bool shouldRepaint(_StickPainter old) =>
      old.x != x || old.y != y || old.dot != dot || old.frame != frame;
}

/// One trigger as a gauge filled to its value.
class _Trigger extends StatelessWidget {
  const _Trigger({required this.label, required this.value});

  final String label;
  final double value;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final text = Theme.of(context).textTheme;
    return Row(
      children: [
        SizedBox(
          width: AppSizes.stickDot * 2,
          child: Text(label, style: text.labelLarge),
        ),
        Expanded(
          child: ClipRRect(
            borderRadius: BorderRadius.circular(AppSizes.radius / 2),
            child: LinearProgressIndicator(
              value: value.clamp(0.0, 1.0),
              minHeight: AppSizes.gauge,
              backgroundColor: scheme.surfaceContainerHighest,
              color: scheme.primary,
            ),
          ),
        ),
        SizedBox(width: AppSizes.gapSmall),
        SizedBox(
          width: AppSizes.stickDot * 2,
          child: Text(
            value.toStringAsFixed(2),
            textAlign: TextAlign.end,
            style: text.bodyMedium?.copyWith(color: context.appColors.muted),
          ),
        ),
      ],
    );
  }
}

/// Every button as a chip, filled while it is down.
class _Buttons extends StatelessWidget {
  const _Buttons({required this.buttons});

  final GamepadButtons buttons;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Wrap(
      spacing: AppSizes.gapSmall,
      runSpacing: AppSizes.gapSmall,
      children: [
        for (var bit = 0; bit < GamepadButtons.names.length; ++bit)
          Chip(
            label: Text(GamepadButtons.names[bit]),
            backgroundColor: buttons.isDown(bit)
                ? scheme.primary
                : scheme.surfaceContainerHighest,
            labelStyle: Theme.of(context).textTheme.labelLarge?.copyWith(
              color: buttons.isDown(bit)
                  ? scheme.onPrimary
                  : scheme.onSurfaceVariant,
            ),
          ),
      ],
    );
  }
}

/// The two haptic tests and what the last one did.
class _Haptics extends StatelessWidget {
  const _Haptics({required this.state});

  final GamepadPageState state;

  @override
  Widget build(BuildContext context) {
    final bloc = context.read<GamepadBloc>();
    final muted = context.appColors.muted;
    final text = Theme.of(context).textTheme;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            Expanded(
              child: FilledButton.icon(
                onPressed: state.gamepad.connected
                    ? () => bloc.add(const GamepadPageRumbleRequested())
                    : null,
                icon: const Icon(Icons.vibration),
                label: const Text('Rumble pad'),
              ),
            ),
            SizedBox(width: AppSizes.gapSmall),
            Expanded(
              child: FilledButton.tonalIcon(
                onPressed: () => bloc.add(const GamepadPageVibrateRequested()),
                icon: const Icon(Icons.smartphone),
                label: const Text('Buzz phone'),
              ),
            ),
          ],
        ),
        if (state.lastHaptic != HapticOutcome.none) ...[
          SizedBox(height: AppSizes.gapSmall),
          Text(
            state.lastHaptic == HapticOutcome.done
                ? 'Haptic sent'
                : 'No vibrator answered',
            textAlign: TextAlign.center,
            style: text.bodyMedium?.copyWith(color: muted),
          ),
        ],
      ],
    );
  }
}
