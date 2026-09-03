import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:mark4/back/drone/drone_manager.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/back/transport/node_kind.dart';
import 'package:mark4/pages/drone/drone_bloc.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/widgets/info_row.dart';
import 'package:mark4/widgets/mark4_app_bar.dart';
import 'package:mark4/widgets/status_banner.dart';

/// One drone: connected to it while the page is open, the link status first.
class DronePage extends StatelessWidget {
  const DronePage({required this.nodeId, super.key});

  final int nodeId;

  @override
  Widget build(BuildContext context) {
    return BlocProvider(
      create: (context) =>
          DroneBloc(drones: context.read<DroneManager>(), nodeId: nodeId)
            ..add(const DroneStarted()),
      child: const _DroneView(),
    );
  }
}

class _DroneView extends StatelessWidget {
  const _DroneView();

  @override
  Widget build(BuildContext context) {
    return BlocBuilder<DroneBloc, DroneState>(
      builder: (context, state) {
        final info = state.connection.info;
        final name = info?.announce.name ?? '';
        return Scaffold(
          appBar: Mark4AppBar(
            title: name.isEmpty ? formatNodeId(state.nodeId) : name,
          ),
          body: Column(
            children: [
              StatusBanner(
                ok: state.isConnected,
                text: switch (state.connection.status) {
                  DroneLinkStatus.connected => 'Connected',
                  DroneLinkStatus.lost when info == null =>
                    'Waiting for the drone',
                  _ => 'Drone disconnected',
                },
              ),
              Expanded(
                child: ListView(
                  padding: EdgeInsets.all(AppSizes.gutter),
                  children: [
                    InfoRow(
                      label: 'Node',
                      value: formatNodeId(state.nodeId),
                      mono: true,
                    ),
                    if (info != null) ..._infoRows(info),
                  ],
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  List<Widget> _infoRows(DroneInfo info) {
    final announce = info.announce;
    return [
      InfoRow(label: 'Kind', value: kindName(announce.kind)),
      InfoRow(label: 'MCU', value: announce.mcu.name),
      InfoRow(label: 'Address', value: info.address, mono: true),
      InfoRow(
        label: 'Build',
        value: announce.gitHash.isEmpty ? 'unknown' : announce.gitHash,
        mono: true,
      ),
      InfoRow(
        label: 'Wire',
        value: announce.wireMismatch ? 'MISMATCH' : 'same schema',
      ),
      InfoRow(
        label: 'Frames',
        value: '${info.received} received, ${info.lost} lost',
      ),
      InfoRow(
        label: 'Last heard',
        value: '${info.lastSeenAgo.inMilliseconds} ms ago',
      ),
    ];
  }
}
