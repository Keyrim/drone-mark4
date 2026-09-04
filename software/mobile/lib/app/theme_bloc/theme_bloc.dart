import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/app/theme_bloc/theme_event.dart';
import 'package:mark4/app/theme_bloc/theme_state.dart';
import 'package:mark4/back/settings/settings_manager.dart';

export 'package:mark4/app/theme_bloc/theme_event.dart';
export 'package:mark4/app/theme_bloc/theme_state.dart';

/// The theme mode of the whole app, above the router: every page's app bar
/// toggles it.
class ThemeBloc extends Bloc<ThemeEvent, ThemeState> {
  ThemeBloc(this._settings) : super(const ThemeState()) {
    on<ThemeStarted>(_onStarted);
    on<ThemeSettingsChanged>(
      (event, emit) => emit(ThemeState(mode: event.mode)),
    );
    on<ThemeToggled>(_onToggled);
  }

  final SettingsManager _settings;
  StreamSubscription<AppThemeMode>? _subscription;

  Future<void> _onStarted(ThemeStarted event, Emitter<ThemeState> emit) async {
    await _subscription?.cancel();
    _subscription = _settings.settings
        .map((settings) => settings.themeMode)
        .distinct()
        .listen((mode) => add(ThemeSettingsChanged(mode)));
  }

  Future<void> _onToggled(ThemeToggled event, Emitter<ThemeState> emit) =>
      _settings.setThemeMode(
        event.displayed == Brightness.dark
            ? AppThemeMode.light
            : AppThemeMode.dark,
      );

  @override
  Future<void> close() async {
    await _subscription?.cancel();
    return super.close();
  }
}
