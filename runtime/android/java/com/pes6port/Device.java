/* PES6PORT — what the touch controls need from Android besides touches (docs/19 §6).
 *
 * gamepad(): the name of a connected gamepad, or null -- asked once a second by
 * runtime/android/main.c, so the touch controls hide while a pad is plugged in, even
 * before its first button. vibrate(): a short tick, the touch controls' feedback when the
 * stick starts sprinting or a flick adds L or R. immersive(): the system bars (status,
 * and the navigation buttons on phones that have them) hidden for good; a swipe from the edge
 * shows them for a moment. Asked again each time the app comes back to the front.
 */
package com.pes6port;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.InputDevice;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

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

    @SuppressWarnings("deprecation")
    public static void immersive(final Activity activity) {
        activity.runOnUiThread(() -> {
            Window w = activity.getWindow();
            if (Build.VERSION.SDK_INT >= 30) {
                w.setDecorFitsSystemWindows(false);
                WindowInsetsController c = w.getInsetsController();
                if (c != null) {
                    c.hide(WindowInsets.Type.systemBars());
                    c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            } else {
                w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
            }
        });
    }

    public static void vibrate(Activity activity, int ms) {
        if (vibrator == null) vibrator = (Vibrator) activity.getSystemService(Context.VIBRATOR_SERVICE);
        if (vibrator == null || !vibrator.hasVibrator()) return;
        vibrator.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
    }
}
