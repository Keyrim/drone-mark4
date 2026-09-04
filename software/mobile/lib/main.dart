import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:logging/logging.dart';
import 'package:mark4/app/app.dart';
import 'package:mark4/back/backend.dart';

/// Boots the back end, then runs the app; a failed boot runs the error page.
Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  Logger.root.level = kDebugMode ? Level.ALL : Level.INFO;
  Logger.root.onRecord.listen(
    (record) => debugPrint(
      '${record.level.name} ${record.loggerName}: ${record.message}',
    ),
  );
  final backend = Backend();
  await backend.init();
  runApp(Mark4App(backend: backend));
}
