import 'package:mark4/gen/mark4.pbenum.dart';

/// The same labels as the hub pages and the editor extension.
String kindName(NodeKind kind) => switch (kind) {
  NodeKind.FIRMWARE => 'firmware',
  NodeKind.DRONE_SIM => 'drone_sim',
  NodeKind.PLANT => 'plant',
  NodeKind.GATEWAY => 'gateway',
  NodeKind.BATCH => 'batch',
  NodeKind.RELAY => 'relay',
  NodeKind.PHONE => 'phone',
  _ => 'unknown',
};

/// A drone is a node one flies: the board, or a desktop flight process.
bool isDroneKind(NodeKind kind) =>
    kind == NodeKind.FIRMWARE || kind == NodeKind.DRONE_SIM;
