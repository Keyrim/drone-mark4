import 'package:equatable/equatable.dart';

final class GamepadStatusState extends Equatable {
  const GamepadStatusState({this.connected = false});

  /// A game controller is present.
  final bool connected;

  @override
  List<Object?> get props => [connected];
}
