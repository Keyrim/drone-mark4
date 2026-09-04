import 'package:logging/logging.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/gamepad/abs_gamepad_source.dart';
import 'package:mark4/back/gamepad/android_gamepad_source.dart';
import 'package:mark4/back/gamepad/gamepad_manager.dart';
import 'package:mark4/back/manager.dart';
import 'package:mark4/back/pilot/pilot_manager.dart';
import 'package:mark4/back/platform/abs_platform.dart';
import 'package:mark4/back/platform/android_platform.dart';
import 'package:mark4/back/settings/settings_manager.dart';
import 'package:mark4/back/transport/ffi_transport_node.dart';
import 'package:mark4/back/transport/transport_manager.dart';

final Logger _log = Logger('app/boot');

/// The composition of the back end, the App class of the C++ executables
/// transposed: every manager a member, dependencies injected by reference,
/// initialized in declaration order and disposed in the reverse order, the
/// first failure stopping the boot.
class Backend {
  Backend._(
    this.platform,
    this.settings,
    this.transport,
    this.drones,
    this.gamepad,
    this.pilot,
  ) : _managers = [settings, transport, drones, gamepad, pilot];

  /// The real composition, or one over fakes when the arguments are given.
  factory Backend({
    AbsPlatform? platform,
    TransportNodeFactory? openNode,
    int Function()? drawNodeId,
    int Function()? clockUs,
    Duration? pollPeriod = const Duration(milliseconds: 10),
    AbsGamepadSource? gamepadSource,
    Duration gamepadStatePeriod = const Duration(milliseconds: 50),
    Duration statusPeriod = const Duration(milliseconds: 50),
    Duration? pilotPeriod = const Duration(milliseconds: 20),
    Duration pilotStatePeriod = const Duration(milliseconds: 50),
  }) {
    final resolvedPlatform = platform ?? AndroidPlatform();
    final settings = SettingsManager();
    final transport = TransportManager(
      platform: resolvedPlatform,
      openNode: openNode ?? FfiTransportNode.open,
      drawNodeId: drawNodeId ?? FfiTransportNode.randomNodeId,
      clockUs: clockUs,
      pollPeriod: pollPeriod,
    );
    final drones = DroneManager(transport, statusPeriod: statusPeriod);
    final gamepad = GamepadManager(
      gamepadSource ?? AndroidGamepadSource(),
      statePeriod: gamepadStatePeriod,
    );
    final pilot = PilotManager(
      gamepad: gamepad,
      drones: drones,
      transport: transport,
      clockUs: clockUs,
      period: pilotPeriod,
      statePeriod: pilotStatePeriod,
    );
    return Backend._(
      resolvedPlatform,
      settings,
      transport,
      drones,
      gamepad,
      pilot,
    );
  }

  final AbsPlatform platform;
  final SettingsManager settings;
  final TransportManager transport;
  final DroneManager drones;
  final GamepadManager gamepad;
  final PilotManager pilot;
  final List<AbsManager> _managers;

  /// Which manager failed, null after a successful init().
  String? bootFailure;

  /// Starts every manager in order. False at the first failure, which is
  /// named in [bootFailure].
  Future<bool> init() async {
    for (final manager in _managers) {
      if (!await manager.init()) {
        bootFailure = manager.runtimeType.toString();
        _log.severe('boot failed in $bootFailure');
        return false;
      }
    }
    _log.info('boot complete');
    return true;
  }

  /// Stops every manager, last started first.
  Future<void> dispose() async {
    for (final manager in _managers.reversed) {
      await manager.dispose();
    }
  }
}
