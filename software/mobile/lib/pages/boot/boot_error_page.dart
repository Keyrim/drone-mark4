import 'package:flutter/material.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/widgets/empty_hint.dart';

/// Shown instead of the app when a manager failed to start.
class BootErrorPage extends StatelessWidget {
  const BootErrorPage({required this.failure, super.key});

  final String failure;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Padding(
        padding: EdgeInsets.all(AppSizes.gutter),
        child: EmptyHint(
          icon: Icons.error_outline,
          text: '$failure failed to start.\nRestart the app.',
        ),
      ),
    );
  }
}
