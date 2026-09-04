import 'package:mark4/back/backend.dart';
import 'package:mark4/back/settings/settings_manager.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'fake_gamepad_source.dart';
import 'fake_platform.dart';
import 'fake_transport_node.dart';

/// The phone's own id in every test.
const int phoneNodeId = 0x0A0B0C0D;

/// A whole back end over fakes: the test drives the network through [node],
/// the controller through [gamepad] and the clock through [nowUs], and
/// polls the transport itself. The gamepad state is published at once by
/// default; a test of the throttle passes its own [gamepadStatePeriod].
class Bench {
  Bench({
    FakePlatform? platform,
    Duration gamepadStatePeriod = Duration.zero,
    Duration pilotStatePeriod = Duration.zero,
  }) : platform = platform ?? FakePlatform(),
       node = FakeTransportNode(phoneNodeId),
       gamepad = FakeGamepadSource() {
    SharedPreferences.setMockInitialValues({});
    backend = Backend(
      platform: this.platform,
      openNode: (_, _) => node,
      drawNodeId: () => phoneNodeId,
      clockUs: () => nowUs,
      pollPeriod: null,
      gamepadSource: gamepad,
      gamepadStatePeriod: gamepadStatePeriod,
      statusPeriod: Duration.zero,
      pilotPeriod: null,
      pilotStatePeriod: pilotStatePeriod,
    );
  }

  final FakePlatform platform;
  final FakeTransportNode node;
  final FakeGamepadSource gamepad;
  late final Backend backend;
  int nowUs = 0;

  SettingsManager get settings => backend.settings;

  /// Boots the back end (the transport timer is not what tests rely on:
  /// they call [poll]).
  Future<void> boot() async {
    final ok = await backend.init();
    if (!ok) {
      throw StateError('boot failed in ${backend.bootFailure}');
    }
  }

  /// Advances the clock, polls the transport once and lets the streams
  /// deliver (a subject hands events to its listeners asynchronously).
  Future<void> poll({int advanceUs = 10000}) async {
    nowUs += advanceUs;
    backend.transport.pollNow();
    await Future<void>.delayed(Duration.zero);
  }

  /// Lets the streams deliver what was added to them.
  Future<void> settle() => Future<void>.delayed(Duration.zero);

  Future<void> dispose() async {
    await backend.dispose();
    await gamepad.close();
  }
}
