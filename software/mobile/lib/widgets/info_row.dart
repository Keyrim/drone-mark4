import 'package:flutter/material.dart';
import 'package:mark4/theme/app_colors.dart';
import 'package:mark4/theme/app_sizes.dart';
import 'package:mark4/theme/app_text.dart';

/// One label and one value, the value large.
class InfoRow extends StatelessWidget {
  const InfoRow({
    required this.label,
    required this.value,
    this.mono = false,
    super.key,
  });

  final String label;
  final String value;
  final bool mono;

  @override
  Widget build(BuildContext context) {
    final text = Theme.of(context).textTheme;
    return Padding(
      padding: EdgeInsets.symmetric(vertical: AppSizes.gapSmall),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.baseline,
        textBaseline: TextBaseline.alphabetic,
        children: [
          Expanded(
            child: Text(
              label,
              style: text.bodyMedium?.copyWith(color: context.appColors.muted),
            ),
          ),
          Expanded(
            flex: 2,
            child: Text(
              value,
              textAlign: TextAlign.end,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: mono
                  ? text.bodyLarge?.copyWith(fontFamily: AppText.monoFamily)
                  : text.bodyLarge,
            ),
          ),
        ],
      ),
    );
  }
}
