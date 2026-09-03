/// A service of the back end: owns a resource (the transport, the stored
/// settings, the drone link), started once by the backend in declaration
/// order and stopped in the reverse order.
///
/// Style of every manager (docs/contributing/dart-guidelines.md): commands
/// are synchronous methods returning a `Future`; what the manager knows is
/// exposed as a `ValueStream` of an `Equatable` value (the current value is
/// readable at once, every change is an event), never as mutable fields.
abstract class AbsManager {
  /// Acquires what the manager needs. The first failure is logged by the
  /// manager itself and reported as false; the backend stops there.
  Future<bool> init();

  /// Releases everything init() acquired. Safe to call after a failed init().
  Future<void> dispose();
}
