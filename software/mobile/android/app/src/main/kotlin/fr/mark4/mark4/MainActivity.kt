package fr.mark4.mark4

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

/**
 * The one activity. Besides hosting Flutter it answers the `mark4/platform`
 * method channel: what the phone provides as an operating system and Flutter
 * does not expose (the Dart side is lib/back/platform/).
 */
class MainActivity : FlutterActivity() {
    private var multicastLock: WifiManager.MulticastLock? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "acquireMulticastLock" -> result.success(acquireMulticastLock())
                "releaseMulticastLock" -> {
                    releaseMulticastLock()
                    result.success(null)
                }
                "deviceName" -> result.success(deviceName())
                else -> result.notImplemented()
            }
        }
    }

    override fun onDestroy() {
        releaseMulticastLock()
        super.onDestroy()
    }

    /**
     * Without this lock Android filters the broadcast datagrams the transport
     * discovers the other nodes with, and the node hears nobody.
     */
    private fun acquireMulticastLock(): Boolean {
        if (multicastLock?.isHeld == true) {
            return true
        }
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return false
        val lock = wifi.createMulticastLock(LOCK_TAG)
        lock.setReferenceCounted(false)
        lock.acquire()
        multicastLock = lock
        return lock.isHeld
    }

    private fun releaseMulticastLock() {
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
    }

    /** The name the user gave the phone in the settings, the model otherwise. */
    private fun deviceName(): String {
        val named = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            Settings.Global.getString(contentResolver, Settings.Global.DEVICE_NAME)
        } else {
            null
        }
        return named?.takeIf { it.isNotBlank() } ?: Build.MODEL
    }

    private companion object {
        const val CHANNEL = "mark4/platform"
        const val LOCK_TAG = "mark4-transport"
    }
}
