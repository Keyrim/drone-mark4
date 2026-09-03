import 'package:flutter/material.dart';
import 'package:mark4/back/transport/node_id.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_text.dart';

/// A node id as the whole system prints it: 8 hex digits, monospace.
class NodeIdText extends StatelessWidget {
  const NodeIdText(this.nodeId, {this.muted = true, super.key});

  final int nodeId;
  final bool muted;

  @override
  Widget build(BuildContext context) {
    final style = AppText.mono(context);
    return Text(
      formatNodeId(nodeId),
      style: muted ? style.copyWith(color: context.appColors.muted) : style,
    );
  }
}
