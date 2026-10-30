/* PES6PORT — what the touch controls need from Android besides touches (docs/19 §6).
 *
 * gamepad(): the name of a connected gamepad, or null -- asked once a second by
 * runtime/android/main.c, so the touch controls hide while a pad is plugged in, even
 * before its first button. vibrate(): a short tick, the touch controls' feedback when the
 * stick starts sprinting or a flick adds L or R.
 */
package com.pes6port;

import android.app.Activity;
import android.content.Context;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.InputDevice;

public final class Device {
    private static Vibrator vibrator;

    public static String gamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                return d.getName();
        }
        return null;
    }

    public static void vibrate(Activity activity, int ms) {
        if (vibrator == null) vibrator = (Vibrator) activity.getSystemService(Context.VIBRATOR_SERVICE);
        if (vibrator == null || !vibrator.hasVibrator()) return;
        vibrator.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
    }
}
