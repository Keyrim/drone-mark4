import 'package:go_router/go_router.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/pages/drone/drone_page.dart';
import 'package:mark4/pages/gamepad/gamepad_page.dart';
import 'package:mark4/pages/home/home_page.dart';

/// Location of the page of one drone.
String droneRoute(int nodeId) => '/drone/${formatNodeId(nodeId)}';

/// Location of the gamepad page.
const String gamepadRoute = '/gamepad';

/// The pages: the home list, one drone by node id, the gamepad.
GoRouter buildRouter() => GoRouter(
  routes: [
    GoRoute(
      path: '/',
      builder: (context, state) => const HomePage(),
      routes: [
        GoRoute(
          path: 'drone/:id',
          redirect: (context, state) =>
              parseNodeId(state.pathParameters['id'] ?? '') == null
              ? '/'
              : null,
          builder: (context, state) =>
              DronePage(nodeId: parseNodeId(state.pathParameters['id']!)!),
        ),
        GoRoute(
          path: 'gamepad',
          builder: (context, state) => const GamepadPage(),
        ),
      ],
    ),
  ],
);
