import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/settings/settings_manager.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  test(
    'the theme mode defaults to the system and is stored when set',
    () async {
      SharedPreferences.setMockInitialValues({});
      final manager = SettingsManager();
      expect(await manager.init(), isTrue);
      expect(manager.settings.value.themeMode, AppThemeMode.system);

      await manager.setThemeMode(AppThemeMode.dark);
      expect(manager.settings.value.themeMode, AppThemeMode.dark);
      await manager.dispose();

      final reloaded = SettingsManager();
      expect(await reloaded.init(), isTrue);
      expect(reloaded.settings.value.themeMode, AppThemeMode.dark);
      await reloaded.dispose();
    },
  );

  test('an unknown stored value falls back to the system', () async {
    SharedPreferences.setMockInitialValues({'themeMode': 'sepia'});
    final manager = SettingsManager();
    expect(await manager.init(), isTrue);
    expect(manager.settings.value.themeMode, AppThemeMode.system);
    await manager.dispose();
  });
}
