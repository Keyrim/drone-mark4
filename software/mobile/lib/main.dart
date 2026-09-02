import 'package:flutter/material.dart';

void main() {
  runApp(const Mark4App());
}

/// Scaffold of the phone-as-gateway app: one screen, nothing behind it yet.
class Mark4App extends StatelessWidget {
  const Mark4App({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      title: 'mark4',
      home: Scaffold(
        body: Center(child: Text('mark4 mobile')),
      ),
    );
  }
}
