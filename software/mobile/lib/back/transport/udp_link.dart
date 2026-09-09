import 'dart:io';
import 'dart:typed_data';

import 'package:logging/logging.dart';
import 'package:mark4/back/transport/frame.dart';

final Logger _log = Logger('back/transport');

/// 255.255.255.255: every node of the LAN.
const int globalBroadcastHost = 0xFFFFFFFF;

/// 127.255.255.255, the broadcast address of the loopback network, which
/// Linux delivers to every local listener.
const int loopbackBroadcastHost = 0x7FFFFFFF;

/// The one UDP port every node of a deployment agrees on.
const int defaultDiscoveryPort = 47820;

/// One frame as it came off the socket, with the address it came from.
class UdpDatagram {
  const UdpDatagram({
    required this.host,
    required this.port,
    required this.bytes,
  });

  /// IPv4 address of the sender, host byte order.
  final int host;

  /// UDP port of the sender.
  final int port;

  /// The frame.
  final Uint8List bytes;
}

/// The phone's one physical link: two UDP sockets, as the C++ UdpLink lays
/// them out. One shared discovery socket every node of the deployment binds
/// and only ever receives broadcasts on, and one ephemeral data socket every
/// frame leaves from, so the source port of any datagram is this node's
/// unicast address. Every method is non-blocking; the Wi-Fi multicast lock
/// the app holds is what lets Android deliver the broadcasts.
class UdpLink {
  UdpLink({this.discoveryPort = defaultDiscoveryPort});

  /// Shared broadcast port of the deployment.
  final int discoveryPort;

  RawDatagramSocket? _discovery;
  RawDatagramSocket? _data;
  final List<int> _localHosts = [];
  bool _loopbackFallback = false;

  /// Port of the unicast socket, 0 before [init].
  int get dataPort => _data?.port ?? 0;

  /// A broadcast had no route and went out on the loopback instead.
  bool get loopbackFallback => _loopbackFallback;

  /// Opens both sockets and lists the host's own addresses, which is how an
  /// echo of this node's own broadcasts is told apart. False when a socket
  /// could not be opened.
  Future<bool> init() async {
    if (_discovery != null || _data != null) {
      return false; // already open
    }
    final discovery = await _bindShared(discoveryPort);
    if (discovery == null) {
      return false;
    }
    _discovery = discovery;
    try {
      _data = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
    } on SocketException catch (error) {
      _log.severe('data socket: $error');
      close();
      return false;
    }
    discovery.broadcastEnabled = true;
    _data!.broadcastEnabled = true;
    await _listLocalHosts();
    return true;
  }

  /// Sends one frame to one peer.
  bool send(Uint8List frame, int host, int port) => _sendTo(frame, host, port);

  /// Sends one frame to every node of the LAN.
  bool broadcast(Uint8List frame) {
    if (_sendTo(frame, globalBroadcastHost, discoveryPort)) {
      return true;
    }
    // Tried again on every send: a network that comes back (a Wi-Fi link,
    // an access point that was down) must not leave the node talking to
    // itself for the rest of its run.
    _loopbackFallback = true;
    return _sendTo(frame, loopbackBroadcastHost, discoveryPort);
  }

  /// Takes one pending frame, if any, without blocking: the unicasts of the
  /// data socket first, then the broadcasts of the discovery socket, the
  /// echoes of this node's own broadcasts skipped.
  UdpDatagram? receive() {
    final unicast = _readOne(_data);
    if (unicast != null) {
      return unicast;
    }
    for (;;) {
      final datagram = _readOne(_discovery);
      if (datagram == null || !_isOwnEcho(datagram)) {
        return datagram;
      }
    }
  }

  /// Closes both sockets.
  void close() {
    _discovery?.close();
    _discovery = null;
    _data?.close();
    _data = null;
    _localHosts.clear();
  }

  /// Dotted IPv4 of a host in host byte order, `192.168.4.1`.
  static String hostText(int host) =>
      '${(host >> 24) & 0xFF}.${(host >> 16) & 0xFF}.'
      '${(host >> 8) & 0xFF}.${host & 0xFF}';

  Future<RawDatagramSocket?> _bindShared(int port) async {
    try {
      return await RawDatagramSocket.bind(
        InternetAddress.anyIPv4,
        port,
        reuseAddress: true,
        reusePort: true,
      );
    } on SocketException catch (error) {
      // Not every platform takes SO_REUSEPORT; without it the port is still
      // shareable with SO_REUSEADDR alone on the systems that need it.
      _log.warning('discovery socket without reusePort: $error');
    }
    try {
      return await RawDatagramSocket.bind(
        InternetAddress.anyIPv4,
        port,
        reuseAddress: true,
      );
    } on SocketException catch (error) {
      _log.severe('cannot bind the discovery port $port: $error');
      return null;
    }
  }

  Future<void> _listLocalHosts() async {
    // Best effort: without the list an echo is merely counted as a
    // duplicate.
    try {
      final interfaces = await NetworkInterface.list(
        type: InternetAddressType.IPv4,
        includeLoopback: true,
      );
      for (final interfaceEntry in interfaces) {
        for (final address in interfaceEntry.addresses) {
          _localHosts.add(_hostOf(address));
        }
      }
    } on OSError catch (error) {
      _log.warning('own addresses unknown: $error');
    } on SocketException catch (error) {
      _log.warning('own addresses unknown: $error');
    }
  }

  bool _isOwnEcho(UdpDatagram datagram) =>
      datagram.port == dataPort && _localHosts.contains(datagram.host);

  UdpDatagram? _readOne(RawDatagramSocket? socket) {
    if (socket == null) {
      return null;
    }
    for (;;) {
      final datagram = socket.receive();
      if (datagram == null) {
        return null;
      }
      if (datagram.data.length > maxFrameSize) {
        continue; // oversized: not one of ours, take the next
      }
      return UdpDatagram(
        host: _hostOf(datagram.address),
        port: datagram.port,
        bytes: datagram.data,
      );
    }
  }

  bool _sendTo(Uint8List frame, int host, int port) {
    final socket = _data;
    if (socket == null || port == 0) {
      return false;
    }
    try {
      return socket.send(frame, _addressOf(host), port) == frame.length;
    } on SocketException {
      return false;
    }
  }

  static int _hostOf(InternetAddress address) {
    final raw = address.rawAddress;
    if (raw.length != 4) {
      return 0;
    }
    return (raw[0] << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
  }

  static InternetAddress _addressOf(int host) => InternetAddress.fromRawAddress(
    Uint8List.fromList([
      (host >> 24) & 0xFF,
      (host >> 16) & 0xFF,
      (host >> 8) & 0xFF,
      host & 0xFF,
    ]),
  );
}
