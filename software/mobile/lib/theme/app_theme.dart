import 'package:flutter/material.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/theme/app_text.dart';

/// The one accent of the app.
const Color accentColor = Color(0xFF1E88E5);

/// The design the sizes are expressed in: a portrait phone.
const Size designSize = Size(390, 844);

/// Builds the theme of one brightness; the app carries both and the theme
/// mode picks.
ThemeData buildTheme(Brightness brightness) {
  final scheme = ColorScheme.fromSeed(
    seedColor: accentColor,
    brightness: brightness,
  );
  final textTheme = AppText.theme();
  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    textTheme: textTheme,
    scaffoldBackgroundColor: scheme.surface,
    appBarTheme: AppBarTheme(
      centerTitle: false,
      backgroundColor: scheme.surface,
      foregroundColor: scheme.onSurface,
      titleTextStyle: textTheme.headlineMedium?.copyWith(
        color: scheme.onSurface,
      ),
      toolbarHeight: AppSizes.appBar,
      iconTheme: IconThemeData(
        size: AppSizes.iconAction,
        color: scheme.onSurface,
      ),
    ),
    listTileTheme: ListTileThemeData(
      minVerticalPadding: AppSizes.gapSmall,
      contentPadding: EdgeInsets.symmetric(horizontal: AppSizes.gutter),
    ),
    cardTheme: CardThemeData(
      margin: EdgeInsets.zero,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(AppSizes.radius),
      ),
    ),
    extensions: [
      brightness == Brightness.dark ? AppColors.dark : AppColors.light,
    ],
  );
}
