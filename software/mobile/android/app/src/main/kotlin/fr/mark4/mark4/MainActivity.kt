package fr.mark4.mark4

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings
import android.view.KeyEvent
import android.view.MotionEvent
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

/**
 * The one activity. Besides hosting Flutter it answers the `mark4/platform`
 * method channel: what the phone provides as an operating system and Flutter
 * does not expose (the Dart side is lib/back/platform/), and it hands every
 * gamepad event to [GamepadBridge] before Flutter sees it.
 */
class MainActivity : FlutterActivity() {
    private var multicastLock: WifiManager.MulticastLock? = null
    /** Null until the engine is configured: an event before that goes to Flutter. */
    private var gamepad: GamepadBridge? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val bridge = GamepadBridge(applicationContext)
        bridge.attach(flutterEngine.dartExecutor.binaryMessenger)
        gamepad = bridge
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "acquireMulticastLock" -> result.success(acquireMulticastLock())
                "releaseMulticastLock" -> {
                    releaseMulticastLock()
                    result.success(null)
                }
                "deviceName" -> result.success(deviceName())
                "gamepadDevices" -> result.success(bridge.devices())
                "gamepadRumble" -> result.success(
                    bridge.rumble(
                        call.argument<Int>("deviceId") ?: -1,
                        call.argument<Int>("durationMs") ?: 0,
                        call.argument<Double>("amplitude") ?: 1.0,
                    ),
                )
                "vibratePhone" -> result.success(bridge.vibratePhone(call.argument<Int>("durationMs") ?: 0))
                else -> result.notImplemented()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Batching input to the display refresh adds up to a frame of
        // latency to every stick sample: turn it off for the gamepad sources.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.requestUnbufferedDispatch(GamepadBridge.GAMEPAD_SOURCES)
        }
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean =
        gamepad?.onMotionEvent(event) == true || super.dispatchGenericMotionEvent(event)

    override fun dispatchKeyEvent(event: KeyEvent): Boolean =
        gamepad?.onKeyEvent(event) == true || super.dispatchKeyEvent(event)

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
