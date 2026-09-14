import 'dart:math';

import 'package:mark4/back/transport/frame.dart';

/// Node ids are self-assigned 32-bit values, printed as 8 hex digits
/// everywhere in the system.
String formatNodeId(int nodeId) =>
    nodeId.toUnsigned(32).toRadixString(16).padLeft(8, '0');

/// Parses what formatNodeId() printed; null when the text is not one.
int? parseNodeId(String text) {
  if (text.isEmpty || text.length > 8) {
    return null;
  }
  return int.tryParse(text, radix: 16);
}

/// Draws this node's id from the operating system's random source, like
/// every desktop process of the system does: never configured, never
/// [broadcastNode], new at every launch.
int randomNodeId() {
  final random = Random.secure();
  var id = broadcastNode;
  while (id == broadcastNode) {
    id = random.nextInt(1 << 32);
  }
  return id;
}
