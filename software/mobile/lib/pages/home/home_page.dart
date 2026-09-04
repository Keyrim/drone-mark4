import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:go_router/go_router.dart';
import 'package:mark4/app/router.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/gamepad/gamepad_manager.dart';
import 'package:mark4/back/transport/transport_manager.dart';
import 'package:mark4/pages/home/home_bloc.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/widgets/drone_tile.dart';
import 'package:mark4/widgets/empty_hint.dart';
import 'package:mark4/widgets/mark4_app_bar.dart';
import 'package:mark4/widgets/node_id_text.dart';

/// The drones on the network; tap one to connect.
class HomePage extends StatelessWidget {
  const HomePage({super.key});

  @override
  Widget build(BuildContext context) {
    return BlocProvider(
      create: (context) => HomeBloc(
        drones: context.read<DroneManager>(),
        transport: context.read<TransportManager>(),
        gamepad: context.read<GamepadManager>(),
      )..add(const HomeStarted()),
      child: const _HomeView(),
    );
  }
}

class _HomeView extends StatelessWidget {
  const _HomeView();

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: Mark4AppBar(title: 'Drones', actions: const [_GamepadAction()]),
      body: BlocBuilder<HomeBloc, HomeState>(
        builder: (context, state) {
          final drones = state.roster.drones;
          if (drones.isEmpty) {
            return const EmptyHint(
              icon: Icons.wifi_find,
              text: 'No drone on the network',
            );
          }
          return ListView.separated(
            itemCount: drones.length,
            separatorBuilder: (_, _) => const Divider(height: 1),
            itemBuilder: (context, index) {
              final drone = drones[index];
              return DroneTile(
                drone: drone,
                onTap: () => context.push(droneRoute(drone.nodeId)),
              );
            },
          );
        },
      ),
      bottomNavigationBar: const _Footer(),
    );
  }
}

/// Opens the gamepad page; lit when a controller is present.
class _GamepadAction extends StatelessWidget {
  const _GamepadAction();

  @override
  Widget build(BuildContext context) {
    return BlocSelector<HomeBloc, HomeState, bool>(
      selector: (state) => state.gamepadConnected,
      builder: (context, connected) => IconButton(
        tooltip: connected ? 'Gamepad connected' : 'No gamepad',
        icon: Icon(
          Icons.sports_esports,
          color: connected ? context.appColors.ok : context.appColors.muted,
        ),
        onPressed: () => context.push(gamepadRoute),
      ),
    );
  }
}

/// This phone's node, and how many other nodes share the network.
class _Footer extends StatelessWidget {
  const _Footer();

  @override
  Widget build(BuildContext context) {
    final text = Theme.of(context).textTheme;
    final muted = context.appColors.muted;
    return BlocBuilder<HomeBloc, HomeState>(
      builder: (context, state) {
        final others = state.roster.otherNodeCount;
        return SafeArea(
          child: Padding(
            padding: EdgeInsets.symmetric(
              horizontal: AppSizes.gutter,
              vertical: AppSizes.gap,
            ),
            child: Row(
              children: [
                Icon(Icons.smartphone, color: muted, size: AppSizes.iconAction),
                SizedBox(width: AppSizes.gapSmall),
                Expanded(
                  child: Text(
                    state.identity.name,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: text.bodyMedium?.copyWith(color: muted),
                  ),
                ),
                NodeIdText(state.identity.nodeId),
                SizedBox(width: AppSizes.gutter),
                Text(
                  others == 1 ? '1 other node' : '$others other nodes',
                  style: text.bodyMedium?.copyWith(color: muted),
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}
