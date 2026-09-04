import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:go_router/go_router.dart';
import 'package:mark4/app/gamepad_status_bloc/gamepad_status_bloc.dart';
import 'package:mark4/app/router.dart';
import 'package:mark4/app/theme_bloc/theme_bloc.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';

/// The app bar of every page: the title, the controller (lit while one is
/// in hand, opens its page) and the light / dark switch.
class Mark4AppBar extends StatelessWidget implements PreferredSizeWidget {
  const Mark4AppBar({
    required this.title,
    this.actions = const [],
    this.showGamepad = true,
    super.key,
  });

  final String title;
  final List<Widget> actions;

  /// The gamepad page hides the shortcut to itself.
  final bool showGamepad;

  @override
  Size get preferredSize => Size.fromHeight(AppSizes.appBar);

  @override
  Widget build(BuildContext context) {
    final brightness = Theme.of(context).brightness;
    return AppBar(
      title: Text(title, maxLines: 1, overflow: TextOverflow.ellipsis),
      actions: [
        ...actions,
        if (showGamepad) const _GamepadAction(),
        IconButton(
          tooltip: brightness == Brightness.dark ? 'Light theme' : 'Dark theme',
          icon: Icon(
            brightness == Brightness.dark ? Icons.light_mode : Icons.dark_mode,
          ),
          onPressed: () =>
              context.read<ThemeBloc>().add(ThemeToggled(brightness)),
        ),
      ],
    );
  }
}

class _GamepadAction extends StatelessWidget {
  const _GamepadAction();

  @override
  Widget build(BuildContext context) {
    return BlocBuilder<GamepadStatusBloc, GamepadStatusState>(
      builder: (context, state) => IconButton(
        tooltip: state.connected ? 'Gamepad connected' : 'No gamepad',
        icon: Icon(
          Icons.sports_esports,
          color: state.connected
              ? context.appColors.ok
              : context.appColors.alert,
        ),
        onPressed: () => context.push(gamepadRoute),
      ),
    );
  }
}
