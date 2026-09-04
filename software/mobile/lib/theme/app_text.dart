import 'package:flutter/material.dart';
import 'package:flutter_screenutil/flutter_screenutil.dart';

/// The type scale, sized for reading from a distance. The family is the
/// platform default for now; changing it is this file.
abstract final class AppText {
  /// Monospace for node ids and hashes.
  static const String monoFamily = 'monospace';

  /// The Material text theme of the app.
  static TextTheme theme() => TextTheme(
    displayLarge: TextStyle(fontSize: 48.sp, fontWeight: FontWeight.w700),
    headlineMedium: TextStyle(fontSize: 32.sp, fontWeight: FontWeight.w600),
    titleLarge: TextStyle(fontSize: 26.sp, fontWeight: FontWeight.w600),
    titleMedium: TextStyle(fontSize: 22.sp, fontWeight: FontWeight.w500),
    bodyLarge: TextStyle(fontSize: 22.sp),
    bodyMedium: TextStyle(fontSize: 18.sp),
    labelLarge: TextStyle(fontSize: 18.sp, fontWeight: FontWeight.w600),
    labelMedium: TextStyle(fontSize: 16.sp),
  );

  /// A node id or a hash: monospace, body size.
  static TextStyle mono(BuildContext context) =>
      Theme.of(context).textTheme.bodyLarge!.copyWith(fontFamily: monoFamily);
}
