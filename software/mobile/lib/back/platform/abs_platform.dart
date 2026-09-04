/// What the phone provides as an operating system and Flutter does not
/// expose. Implemented over the `mark4/platform` method channel of the
/// Android activity; faked in tests.
abstract class AbsPlatform {
  /// Holds the Wi-Fi multicast lock; without it Android drops the incoming
  /// broadcast datagrams and the transport hears nobody.
  /// Returns false when the lock could not be taken.
  Future<bool> acquireMulticastLock();

  /// Releases the multicast lock, if held.
  Future<void> releaseMulticastLock();

  /// The name the user gave the phone in the settings, the model otherwise.
  Future<String> deviceName();
}
