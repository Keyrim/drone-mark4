import 'package:flutter/material.dart';

/// The colors the Material scheme has no name for, one set per brightness,
/// reached with `context.appColors`.
class AppColors extends ThemeExtension<AppColors> {
  const AppColors({
    required this.ok,
    required this.alert,
    required this.muted,
    required this.onStatus,
  });

  static const AppColors light = AppColors(
    ok: Color(0xFF1B7F3B),
    alert: Color(0xFFC62828),
    muted: Color(0xFF5F6368),
    onStatus: Color(0xFFFFFFFF),
  );

  static const AppColors dark = AppColors(
    ok: Color(0xFF4CCB6A),
    alert: Color(0xFFFF6B6B),
    muted: Color(0xFF9AA0A6),
    onStatus: Color(0xFF101010),
  );

  /// The link is up, the value is nominal.
  final Color ok;

  /// The link is lost, something needs a look.
  final Color alert;

  /// Secondary text.
  final Color muted;

  /// Text and icons drawn on [ok] or [alert].
  final Color onStatus;

  @override
  AppColors copyWith({
    Color? ok,
    Color? alert,
    Color? muted,
    Color? onStatus,
  }) => AppColors(
    ok: ok ?? this.ok,
    alert: alert ?? this.alert,
    muted: muted ?? this.muted,
    onStatus: onStatus ?? this.onStatus,
  );

  @override
  AppColors lerp(AppColors? other, double t) {
    if (other == null) {
      return this;
    }
    return AppColors(
      ok: Color.lerp(ok, other.ok, t)!,
      alert: Color.lerp(alert, other.alert, t)!,
      muted: Color.lerp(muted, other.muted, t)!,
      onStatus: Color.lerp(onStatus, other.onStatus, t)!,
    );
  }
}

extension AppColorsContext on BuildContext {
  /// The extension of the current theme.
  AppColors get appColors => Theme.of(this).extension<AppColors>()!;
}
