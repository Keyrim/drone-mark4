import 'package:flutter_test/flutter_test.dart';
import 'package:mark4/back/transport/node_id.dart';

void main() {
  test('a node id prints as 8 hex digits', () {
    expect(formatNodeId(0x1A2B), '00001a2b');
    expect(formatNodeId(0xDEADBEEF), 'deadbeef');
  });

  test('parsing accepts what printing produced and refuses the rest', () {
    expect(parseNodeId('deadbeef'), 0xDEADBEEF);
    expect(parseNodeId('00001a2b'), 0x1A2B);
    expect(parseNodeId(''), isNull);
    expect(parseNodeId('not-hex'), isNull);
    expect(parseNodeId('123456789'), isNull);
  });
}
