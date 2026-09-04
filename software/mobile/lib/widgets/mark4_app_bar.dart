import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/app/theme_bloc/theme_bloc.dart';
import 'package:mark4/theme/app_sizes.dart';

/// The app bar of every page: the title, and the light / dark switch.
class Mark4AppBar extends StatelessWidget implements PreferredSizeWidget {
  const Mark4AppBar({required this.title, this.actions = const [], super.key});

  final String title;
  final List<Widget> actions;

  @override
  Size get preferredSize => Size.fromHeight(AppSizes.appBar);

  @override
  Widget build(BuildContext context) {
    final brightness = Theme.of(context).brightness;
    return AppBar(
      title: Text(title, maxLines: 1, overflow: TextOverflow.ellipsis),
      actions: [
        ...actions,
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
