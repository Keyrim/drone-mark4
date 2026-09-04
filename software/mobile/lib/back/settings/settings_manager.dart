import 'package:equatable/equatable.dart';
import 'package:logging/logging.dart';
import 'package:mark4/back/manager.dart';
import 'package:rxdart/rxdart.dart';
import 'package:shared_preferences/shared_preferences.dart';

final Logger _log = Logger('back/settings');

/// The theme the user wants: the phone's own setting, or one forced. The
/// bench at night wants dark, the field in daylight wants light, and the
/// switch is on every page.
enum AppThemeMode { system, light, dark }

/// Everything the user set, persisted on the phone.
class Settings extends Equatable {
  const Settings({this.themeMode = AppThemeMode.system});

  final AppThemeMode themeMode;

  Settings copyWith({AppThemeMode? themeMode}) =>
      Settings(themeMode: themeMode ?? this.themeMode);

  @override
  List<Object?> get props => [themeMode];
}

/// Owns the persisted settings.
class SettingsManager extends AbsManager {
  static const String _themeModeKey = 'themeMode';

  final BehaviorSubject<Settings> _settings = BehaviorSubject.seeded(
    const Settings(),
  );
  SharedPreferences? _store;

  /// The settings, the stored ones once init() ran.
  ValueStream<Settings> get settings => _settings.stream;

  @override
  Future<bool> init() async {
    final store = await SharedPreferences.getInstance();
    _store = store;
    final stored = store.getString(_themeModeKey);
    final themeMode =
        AppThemeMode.values.asNameMap()[stored] ?? AppThemeMode.system;
    _settings.add(Settings(themeMode: themeMode));
    _log.info('settings loaded: theme ${themeMode.name}');
    return true;
  }

  @override
  Future<void> dispose() async {
    await _settings.close();
  }

  /// Applies and stores the theme mode.
  Future<void> setThemeMode(AppThemeMode mode) async {
    if (mode == _settings.value.themeMode) {
      return;
    }
    _settings.add(_settings.value.copyWith(themeMode: mode));
    await _store?.setString(_themeModeKey, mode.name);
  }
}
