import 'package:equatable/equatable.dart';
import 'package:flutter/material.dart';
import 'package:mark4/back/settings/settings_manager.dart';

final class ThemeState extends Equatable {
  const ThemeState({this.mode = AppThemeMode.system});

  final AppThemeMode mode;

  /// What MaterialApp takes.
  ThemeMode get themeMode => switch (mode) {
    AppThemeMode.system => ThemeMode.system,
    AppThemeMode.light => ThemeMode.light,
    AppThemeMode.dark => ThemeMode.dark,
  };

  @override
  List<Object?> get props => [mode];
}
