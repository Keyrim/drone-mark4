package fr.mark4.mark4

import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.os.CombinedVibration
import android.os.Handler
import android.os.Looper
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel

/**
 * The gamepad as Android exposes it: input events on the activity, an input
 * device in the system's table, a vibrator or two. Android reserves the BLE
 * HID service to itself, so this is the only way to read the controller;
 * the Dart side (lib/back/gamepad/) sees one event stream on the
 * `mark4/gamepad` channel and a few methods on `mark4/platform`.
 *
 * Two kinds of events travel on the stream: a sample, packed as a
 * Float64List (see [SAMPLE_SIZE] and the indices below; the Dart decoder
 * mirrors them), and the device list, a List of Maps, sent when the stream
 * is listened to and every time a gamepad appears, disappears or changes.
 *
 * Every key and motion event of a gamepad source is consumed here: left to
 * Flutter, the hat becomes DPAD focus traversal and a button a text entry.
 * BACK is the one exception, so the system gesture still works.
 */
class GamepadBridge(private val context: Context) : EventChannel.StreamHandler, InputManager.InputDeviceListener {
    private val inputManager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private var sink: EventChannel.EventSink? = null

    /** Last axes and buttons seen per device id: a key event updates the buttons and repeats the axes. */
    private val lastSample = HashMap<Int, DoubleArray>()

    fun attach(messenger: BinaryMessenger) {
        EventChannel(messenger, STREAM_CHANNEL).setStreamHandler(this)
    }

    // ---- EventChannel.StreamHandler

    override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
        sink = events
        inputManager.registerInputDeviceListener(this, mainHandler)
        events.success(devices())
    }

    override fun onCancel(arguments: Any?) {
        inputManager.unregisterInputDeviceListener(this)
        sink = null
    }

    // ---- InputManager.InputDeviceListener

    override fun onInputDeviceAdded(deviceId: Int) = publishDevices()

    override fun onInputDeviceRemoved(deviceId: Int) {
        lastSample.remove(deviceId)
        publishDevices()
    }

    override fun onInputDeviceChanged(deviceId: Int) = publishDevices()

    private fun publishDevices() {
        sink?.success(devices())
    }

    /** The connected gamepads, one Map each: id, name, whether it can rumble. */
    fun devices(): List<Map<String, Any>> =
        InputDevice.getDeviceIds()
            .asList()
            .mapNotNull { id -> InputDevice.getDevice(id) }
            .filter { isGamepad(it) && !it.isVirtual }
            .map { device ->
                mapOf(
                    "id" to device.id,
                    "name" to device.name,
                    "rumble" to hasVibrator(device),
                )
            }

    // ---- input events, called from the activity's dispatch overrides

    /**
     * Takes a motion event; true when it came from a gamepad and was consumed.
     * The batched history is drained first, oldest sample first, so nothing
     * the controller reported is skipped when the input dispatcher coalesced.
     */
    fun onMotionEvent(event: MotionEvent): Boolean {
        if (!isGamepadSource(event.source)) {
            return false
        }
        val buttons = lastSample[event.deviceId]?.get(BUTTONS) ?: 0.0
        for (position in 0 until event.historySize) {
            emit(packMotion(event, position, buttons))
        }
        emit(packMotion(event, -1, buttons))
        return true
    }

    /**
     * Takes a key event; true when it came from a gamepad and was consumed.
     * Repeats are ignored (a held button is a level, not a stream of
     * presses) and BACK is left to the system.
     */
    fun onKeyEvent(event: KeyEvent): Boolean {
        if (!isGamepadSource(event.source) && !isGamepadKey(event.keyCode)) {
            return false
        }
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            return false
        }
        val bit = buttonBit(event.keyCode) ?: return true
        if (event.repeatCount > 0) {
            return true
        }
        val sample = lastSample[event.deviceId]?.copyOf() ?: DoubleArray(SAMPLE_SIZE).also {
            it[DEVICE_ID] = event.deviceId.toDouble()
        }
        val mask = sample[BUTTONS].toLong()
        sample[BUTTONS] = when (event.action) {
            KeyEvent.ACTION_DOWN -> mask or (1L shl bit)
            KeyEvent.ACTION_UP -> mask and (1L shl bit).inv()
            else -> mask
        }.toDouble()
        sample[EVENT_TIME_MS] = event.eventTime.toDouble()
        emit(sample)
        return true
    }

    /**
     * Rumbles one gamepad for [durationMs] at [amplitude] in (0, 1]; false
     * when the device is gone or has no vibrator.
     */
    fun rumble(deviceId: Int, durationMs: Int, amplitude: Double): Boolean {
        val device = InputDevice.getDevice(deviceId) ?: return false
        val strength = (amplitude.coerceIn(0.0, 1.0) * 255).toInt().coerceIn(1, 255)
        val effect = VibrationEffect.createOneShot(durationMs.toLong(), strength)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val manager = device.vibratorManager
            if (manager.vibratorIds.isEmpty()) {
                return false
            }
            manager.vibrate(CombinedVibration.createParallel(effect))
            return true
        }
        @Suppress("DEPRECATION")
        val vibrator = device.vibrator
        if (!vibrator.hasVibrator()) {
            return false
        }
        vibrator.vibrate(effect)
        return true
    }

    /**
     * Vibrates the phone itself for [durationMs]. Filed as an alarm: under
     * the touch feedback usage Android drops it whenever that setting is
     * off, and this is the fallback of a lost link, not a key click.
     */
    fun vibratePhone(durationMs: Int): Boolean {
        val effect = VibrationEffect.createOneShot(durationMs.toLong(), VibrationEffect.DEFAULT_AMPLITUDE)
        val vibrator: Vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager).defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
        }
        if (!vibrator.hasVibrator()) {
            return false
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            vibrator.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_ALARM))
        } else {
            @Suppress("DEPRECATION")
            vibrator.vibrate(effect)
        }
        return true
    }

    // ---- packing

    private fun packMotion(event: MotionEvent, position: Int, buttons: Double): DoubleArray {
        val sample = DoubleArray(SAMPLE_SIZE)
        sample[DEVICE_ID] = event.deviceId.toDouble()
        sample[EVENT_TIME_MS] = (if (position < 0) event.eventTime else event.getHistoricalEventTime(position)).toDouble()
        sample[LEFT_X] = axis(event, MotionEvent.AXIS_X, position)
        sample[LEFT_Y] = axis(event, MotionEvent.AXIS_Y, position)
        sample[RIGHT_X] = axis(event, MotionEvent.AXIS_Z, position)
        sample[RIGHT_Y] = axis(event, MotionEvent.AXIS_RZ, position)
        // Xbox controllers report the triggers on BRAKE / GAS, other pads on
        // LTRIGGER / RTRIGGER: whichever is non zero is the trigger.
        sample[LEFT_TRIGGER] = maxOf(axis(event, MotionEvent.AXIS_BRAKE, position), axis(event, MotionEvent.AXIS_LTRIGGER, position))
        sample[RIGHT_TRIGGER] = maxOf(axis(event, MotionEvent.AXIS_GAS, position), axis(event, MotionEvent.AXIS_RTRIGGER, position))
        sample[HAT_X] = axis(event, MotionEvent.AXIS_HAT_X, position)
        sample[HAT_Y] = axis(event, MotionEvent.AXIS_HAT_Y, position)
        sample[BUTTONS] = buttons
        return sample
    }

    private fun axis(event: MotionEvent, axis: Int, position: Int): Double =
        (if (position < 0) event.getAxisValue(axis) else event.getHistoricalAxisValue(axis, position)).toDouble()

    private fun emit(sample: DoubleArray) {
        lastSample[sample[DEVICE_ID].toInt()] = sample
        sink?.success(sample)
    }

    companion object {
        const val STREAM_CHANNEL = "mark4/gamepad"

        /** Bit of each button in the mask; the Dart GamepadButtons mirrors it. */
        const val BIT_A = 0
        const val BIT_B = 1
        const val BIT_X = 2
        const val BIT_Y = 3
        const val BIT_LB = 4
        const val BIT_RB = 5
        const val BIT_VIEW = 6
        const val BIT_MENU = 7
        const val BIT_LEFT_THUMB = 8
        const val BIT_RIGHT_THUMB = 9
        const val BIT_DPAD_UP = 10
        const val BIT_DPAD_DOWN = 11
        const val BIT_DPAD_LEFT = 12
        const val BIT_DPAD_RIGHT = 13
        const val BIT_GUIDE = 14

        /** Layout of one packed sample; the Dart GamepadSample decoder mirrors it. */
        const val DEVICE_ID = 0
        const val EVENT_TIME_MS = 1
        const val LEFT_X = 2
        const val LEFT_Y = 3
        const val RIGHT_X = 4
        const val RIGHT_Y = 5
        const val LEFT_TRIGGER = 6
        const val RIGHT_TRIGGER = 7
        const val HAT_X = 8
        const val HAT_Y = 9
        const val BUTTONS = 10
        const val SAMPLE_SIZE = 11

        /** The sources that mean "a game controller": pads set either, some both. */
        const val GAMEPAD_SOURCES = InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_JOYSTICK

        /**
         * Whole-mask test on purpose: every source carries its class in its
         * low bits (SOURCE_GAMEPAD is a button class, SOURCE_JOYSTICK a
         * joystick class), so a non-zero `and` also matches the keyboard of
         * the phone's own buttons, its fingerprint sensor and its touch panel.
         */
        fun isGamepadSource(source: Int): Boolean =
            source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK

        fun isGamepad(device: InputDevice): Boolean = isGamepadSource(device.sources)

        private fun isGamepadKey(keyCode: Int): Boolean = KeyEvent.isGamepadButton(keyCode)

        private fun buttonBit(keyCode: Int): Int? = when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> BIT_A
            KeyEvent.KEYCODE_BUTTON_B -> BIT_B
            KeyEvent.KEYCODE_BUTTON_X -> BIT_X
            KeyEvent.KEYCODE_BUTTON_Y -> BIT_Y
            KeyEvent.KEYCODE_BUTTON_L1 -> BIT_LB
            KeyEvent.KEYCODE_BUTTON_R1 -> BIT_RB
            KeyEvent.KEYCODE_BUTTON_SELECT -> BIT_VIEW
            KeyEvent.KEYCODE_BUTTON_START -> BIT_MENU
            KeyEvent.KEYCODE_BUTTON_THUMBL -> BIT_LEFT_THUMB
            KeyEvent.KEYCODE_BUTTON_THUMBR -> BIT_RIGHT_THUMB
            KeyEvent.KEYCODE_DPAD_UP -> BIT_DPAD_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> BIT_DPAD_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> BIT_DPAD_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> BIT_DPAD_RIGHT
            KeyEvent.KEYCODE_BUTTON_MODE -> BIT_GUIDE
            else -> null
        }

        private fun hasVibrator(device: InputDevice): Boolean =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                device.vibratorManager.vibratorIds.isNotEmpty()
            } else {
                @Suppress("DEPRECATION")
                device.vibrator.hasVibrator()
            }
    }
}
