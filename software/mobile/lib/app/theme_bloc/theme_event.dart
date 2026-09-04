import 'package:equatable/equatable.dart';
import 'package:flutter/material.dart';
import 'package:mark4/back/settings/settings_manager.dart';

sealed class ThemeEvent extends Equatable {
  const ThemeEvent();

  @override
  List<Object?> get props => [];
}

/// Start following the settings.
final class ThemeStarted extends ThemeEvent {
  const ThemeStarted();
}

/// The stored settings changed.
final class ThemeSettingsChanged extends ThemeEvent {
  const ThemeSettingsChanged(this.mode);

  final AppThemeMode mode;

  @override
  List<Object?> get props => [mode];
}

/// The user tapped the switch: force the opposite of what is displayed.
final class ThemeToggled extends ThemeEvent {
  const ThemeToggled(this.displayed);

  final Brightness displayed;

  @override
  List<Object?> get props => [displayed];
}
