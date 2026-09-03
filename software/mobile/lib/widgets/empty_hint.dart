import 'package:flutter/material.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';

/// What a page shows when it has nothing to list.
class EmptyHint extends StatelessWidget {
  const EmptyHint({required this.icon, required this.text, super.key});

  final IconData icon;
  final String text;

  @override
  Widget build(BuildContext context) {
    final muted = context.appColors.muted;
    return Center(
      child: Padding(
        padding: EdgeInsets.all(AppSizes.gutter),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: AppSizes.icon * 2, color: muted),
            SizedBox(height: AppSizes.gap),
            Text(
              text,
              textAlign: TextAlign.center,
              style: Theme.of(
                context,
              ).textTheme.titleMedium?.copyWith(color: muted),
            ),
          ],
        ),
      ),
    );
  }
}
