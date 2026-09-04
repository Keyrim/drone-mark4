import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:flutter_screenutil/flutter_screenutil.dart';
import 'package:go_router/go_router.dart';
import 'package:mark4/app/gamepad_status_bloc/gamepad_status_bloc.dart';
import 'package:mark4/app/router.dart';
import 'package:mark4/app/theme_bloc/theme_bloc.dart';
import 'package:mark4/back/backend.dart';
import 'package:mark4/pages/boot/boot_error_page.dart';
import 'package:mark4/theme/app_theme.dart';

/// The widget tree above the pages: the managers made reachable, the theme
/// bloc, screen_util, the router.
class Mark4App extends StatefulWidget {
  const Mark4App({required this.backend, super.key});

  final Backend backend;

  @override
  State<Mark4App> createState() => _Mark4AppState();
}

class _Mark4AppState extends State<Mark4App> {
  late final GoRouter _router = buildRouter();

  @override
  Widget build(BuildContext context) {
    final failure = widget.backend.bootFailure;
    return ScreenUtilInit(
      designSize: designSize,
      minTextAdapt: true,
      builder: (context, child) {
        if (failure != null) {
          return MaterialApp(
            title: 'mark4',
            theme: buildTheme(Brightness.light),
            darkTheme: buildTheme(Brightness.dark),
            home: BootErrorPage(failure: failure),
          );
        }
        return MultiRepositoryProvider(
          providers: [
            RepositoryProvider.value(value: widget.backend.settings),
            RepositoryProvider.value(value: widget.backend.transport),
            RepositoryProvider.value(value: widget.backend.drones),
            RepositoryProvider.value(value: widget.backend.gamepad),
            RepositoryProvider.value(value: widget.backend.pilot),
          ],
          child: MultiBlocProvider(
            providers: [
              BlocProvider(
                create: (_) =>
                    ThemeBloc(widget.backend.settings)
                      ..add(const ThemeStarted()),
              ),
              BlocProvider(
                create: (_) =>
                    GamepadStatusBloc(widget.backend.gamepad)
                      ..add(const GamepadStatusStarted()),
              ),
            ],
            child: BlocBuilder<ThemeBloc, ThemeState>(
              builder: (context, state) => MaterialApp.router(
                title: 'mark4',
                theme: buildTheme(Brightness.light),
                darkTheme: buildTheme(Brightness.dark),
                themeMode: state.themeMode,
                routerConfig: _router,
              ),
            ),
          ),
        );
      },
    );
  }
}
