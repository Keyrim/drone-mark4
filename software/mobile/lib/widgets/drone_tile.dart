import 'package:flutter/material.dart';
import 'package:mark4/back/drone/drone_models.dart';
import 'package:mark4/back/transport/node_kind.dart';
import 'package:mark4/gen/mark4.pbenum.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/widgets/node_id_text.dart';

/// One drone of the home list: name, id, kind, tap to connect.
class DroneTile extends StatelessWidget {
  const DroneTile({required this.drone, required this.onTap, super.key});

  final DroneSummary drone;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final text = Theme.of(context).textTheme;
    final isBoard = drone.kind == NodeKind.FIRMWARE;
    return SizedBox(
      height: AppSizes.rowHeight,
      child: ListTile(
        onTap: onTap,
        leading: Icon(
          isBoard ? Icons.flight : Icons.computer,
          size: AppSizes.icon,
        ),
        title: Text(
          drone.name.isEmpty ? kindName(drone.kind) : drone.name,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: text.titleLarge,
        ),
        subtitle: Row(
          children: [
            NodeIdText(drone.nodeId),
            SizedBox(width: AppSizes.gutter),
            Text(
              kindName(drone.kind),
              style: text.bodyMedium?.copyWith(color: context.appColors.muted),
            ),
          ],
        ),
        trailing: Icon(Icons.chevron_right, size: AppSizes.icon),
      ),
    );
  }
}
