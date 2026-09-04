import 'package:flutter_screenutil/flutter_screenutil.dart';

/// Every dimension of the pages, scaled from the design size by screen_util.
/// The rule of the app: few things on screen, large, readable from a
/// distance with the phone set down and the hands on the controller. Nothing
/// is sized in a widget; a new size is a new name here.
abstract final class AppSizes {
  /// Horizontal margin of a page.
  static double get gutter => 20.w;

  /// Vertical space between two blocks.
  static double get gap => 16.h;

  /// Vertical space between two lines of one block.
  static double get gapSmall => 8.h;

  /// Height of one tappable row of a list.
  static double get rowHeight => 96.h;

  /// Corner radius of cards, banners and tiles.
  static double get radius => 16.r;

  /// Icons next to a title.
  static double get icon => 32.sp;

  /// Icons of the app bar actions.
  static double get iconAction => 28.sp;

  /// Height of the status banner of a page.
  static double get banner => 72.h;

  /// Height of the app bar.
  static double get appBar => 72.h;

  /// Side of the square a stick moves in.
  static double get stickPad => 150.w;

  /// Diameter of the dot marking a stick position.
  static double get stickDot => 22.w;

  /// Height of a trigger gauge.
  static double get gauge => 20.h;

  /// Width of a stroke drawn around a pad or a gauge.
  static double get stroke => 2.w;

  /// Height of the phase band of the cockpit.
  static double get phaseBand => 110.h;

  /// Height of one motor bar.
  static double get motorBar => 14.h;

  /// Side of the horizon square.
  static double get horizon => 120.w;

  /// Diameter of a link indicator dot.
  static double get linkDot => 16.w;
}
