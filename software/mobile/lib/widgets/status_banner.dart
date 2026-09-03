import 'package:flutter/material.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';

/// Full-width line at the top of a page saying whether things are fine.
class StatusBanner extends StatelessWidget {
  const StatusBanner({required this.text, required this.ok, super.key});

  final String text;
  final bool ok;

  @override
  Widget build(BuildContext context) {
    final colors = context.appColors;
    final color = ok ? colors.ok : colors.alert;
    final onColor = colors.onStatus;
    return Container(
      height: AppSizes.banner,
      width: double.infinity,
      color: color,
      padding: EdgeInsets.symmetric(horizontal: AppSizes.gutter),
      child: Row(
        children: [
          Icon(
            ok ? Icons.link : Icons.link_off,
            size: AppSizes.icon,
            color: onColor,
          ),
          SizedBox(width: AppSizes.gutter),
          Expanded(
            child: Text(
              text,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: Theme.of(
                context,
              ).textTheme.titleLarge?.copyWith(color: onColor),
            ),
          ),
        ],
      ),
    );
  }
}
