/* PES6PORT — choosing the player's own PES6 ISO (docs/35).
 *
 * The pre-game menu's ISO row (runtime/common/launcher.c) asks the host to pick
 * a file; the host calls pick(), which starts this invisible activity, which
 * opens the system's document picker. The host polls state() and then takes
 * the file through openFd(): a descriptor it reads (and checks, and copies into
 * the app's files) itself. NativeActivity gets no activity results, hence the
 * separate activity.
 */
package com.pes6port;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

public final class IsoPicker extends Activity {
    /* 0 nothing asked, 1 the picker is open, 2 a file was chosen, 3 cancelled */
    private static volatile int state;
    private static volatile Uri chosen;
    private static volatile String chosenName = "";

    public static void pick(final Activity activity) {
        state = 1;
        activity.runOnUiThread(() -> activity.startActivity(new Intent(activity, IsoPicker.class)));
    }

    public static int state() { return state; }

    public static String name() { return chosenName; }

    /* A read-only descriptor of the chosen file (the caller closes it), or -1. */
    public static int openFd(Activity activity) {
        try {
            ParcelFileDescriptor p = activity.getContentResolver().openFileDescriptor(chosen, "r");
            return p == null ? -1 : p.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    public static long size(Activity activity) {
        try (Cursor c = activity.getContentResolver().query(chosen, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(OpenableColumns.SIZE);
                if (i >= 0 && !c.isNull(i)) return c.getLong(i);
            }
        } catch (Exception e) {
            // unknown: the copy then runs to the end of the file
        }
        return -1;
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) return;                       /* recreated: the picker is already up */
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");                                /* .iso has no MIME type of its own */
        try {
            startActivityForResult(i, 1);
        } catch (Exception e) {
            state = 3;
            finish();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (result == RESULT_OK && data != null && data.getData() != null) {
            chosen = data.getData();
            chosenName = displayName(chosen);
            state = 2;
        } else {
            state = 3;
        }
        finish();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (i >= 0 && !c.isNull(i)) return c.getString(i);
            }
        } catch (Exception e) {
            // fall through
        }
        String s = uri.getLastPathSegment();
        return s == null ? "" : s;
    }
}
