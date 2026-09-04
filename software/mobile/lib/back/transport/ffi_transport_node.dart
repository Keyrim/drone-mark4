import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/gen/transport_bindings.dart';

/// The real node: the project's C++ transport, compiled into
/// libmark4_transport.so by native/CMakeLists.txt, behind the C ABI of
/// native/include/mark4/transport_shim.h and its ffigen binding.
class FfiTransportNode implements AbsTransportNode {
  FfiTransportNode._(this._bindings, this._handle)
    : _rxBuffer = calloc<Uint8>(MARK4_MAX_PAYLOAD),
      _txBuffer = calloc<Uint8>(MARK4_MAX_PAYLOAD),
      _srcOut = calloc<Uint32>(),
      _nodeOut = calloc<Mark4NodeInfo>(),
      _statsOut = calloc<Mark4TransportStats>();

  static TransportBindings? _sharedBindings;

  /// The binding, loaded on first use.
  static TransportBindings get bindings => _sharedBindings ??=
      TransportBindings(DynamicLibrary.open('libmark4_transport.so'));

  /// Draws a node id from the OS random source; 0 when it cannot be read.
  static int randomNodeId() => bindings.mark4_random_node_id();

  /// Opens one node on [discoveryPort]; null when the sockets could not be
  /// opened or [nodeId] is 0.
  static FfiTransportNode? open(int nodeId, int discoveryPort) {
    final handle = bindings.mark4_transport_create(nodeId, discoveryPort);
    if (handle == nullptr) {
      return null;
    }
    return FfiTransportNode._(bindings, handle);
  }

  final TransportBindings _bindings;
  Pointer<Mark4Transport> _handle;
  final Pointer<Uint8> _rxBuffer;
  final Pointer<Uint8> _txBuffer;
  final Pointer<Uint32> _srcOut;
  final Pointer<Mark4NodeInfo> _nodeOut;
  final Pointer<Mark4TransportStats> _statsOut;

  @override
  int get nodeId => _bindings.mark4_transport_node_id(_handle);

  @override
  bool setBeacon(Uint8List payload) {
    if (payload.length > MARK4_MAX_BEACON_SIZE) {
      return false;
    }
    _txBuffer.asTypedList(MARK4_MAX_PAYLOAD).setAll(0, payload);
    return _bindings.mark4_transport_set_beacon(
      _handle,
      _txBuffer,
      payload.length,
    );
  }

  @override
  bool send(int dst, Uint8List payload) {
    if (payload.length > MARK4_MAX_PAYLOAD) {
      return false;
    }
    _txBuffer.asTypedList(MARK4_MAX_PAYLOAD).setAll(0, payload);
    return _bindings.mark4_transport_send(
      _handle,
      dst,
      _txBuffer,
      payload.length,
    );
  }

  @override
  int poll(int nowUs) => _bindings.mark4_transport_poll(_handle, nowUs);

  @override
  InboundPayload? nextPayload() {
    final size = _bindings.mark4_transport_next_payload(
      _handle,
      _srcOut,
      _rxBuffer,
      MARK4_MAX_PAYLOAD,
    );
    if (size == 0) {
      return null;
    }
    return InboundPayload(
      src: _srcOut.value,
      bytes: Uint8List.fromList(_rxBuffer.asTypedList(size)),
    );
  }

  @override
  List<NodeInfo> nodes() {
    final count = _bindings.mark4_transport_node_count(_handle);
    final result = <NodeInfo>[];
    for (var index = 0; index < count; ++index) {
      if (!_bindings.mark4_transport_node_at(_handle, index, _nodeOut)) {
        break;
      }
      final node = _nodeOut.ref;
      result.add(
        NodeInfo(
          id: node.id,
          host: node.host,
          port: node.port,
          lastSeenUs: node.last_seen_us,
          received: node.received,
          lost: node.lost,
          duplicates: node.duplicates,
        ),
      );
    }
    return result;
  }

  @override
  TransportStats stats() {
    _bindings.mark4_transport_stats(_handle, _statsOut);
    final stats = _statsOut.ref;
    return TransportStats(
      sent: stats.sent,
      sentBytes: stats.sent_bytes,
      refused: stats.refused,
      dropped: stats.dropped,
      rxOverflow: stats.rx_overflow,
      dataPort: stats.data_port,
      loopbackFallback: stats.loopback_fallback,
    );
  }

  @override
  void dispose() {
    if (_handle == nullptr) {
      return;
    }
    _bindings.mark4_transport_destroy(_handle);
    _handle = nullptr;
    calloc.free(_rxBuffer);
    calloc.free(_txBuffer);
    calloc.free(_srcOut);
    calloc.free(_nodeOut);
    calloc.free(_statsOut);
  }
}
