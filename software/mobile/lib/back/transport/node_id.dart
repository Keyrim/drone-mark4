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

/// The node every node hears: destination of a broadcast.
const int broadcastNode = 0;
