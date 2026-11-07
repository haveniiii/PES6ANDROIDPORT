/* PES6PORT — the PSP on-screen keyboard as an Android dialog (runtime/common/osk.c).
 *
 * The game asks for a WIRELESS player-file name; this shows a dialog with a
 * text field and the phone's own keyboard, and hands the answer back through
 * done(), a native method the host registers (runtime/android/main.c). Only
 * printable ASCII is accepted: the game's font has nothing else.
 */
package com.pes6port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.text.InputFilter;
import android.text.InputType;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;

public final class TextPrompt {
    private static native void done(boolean ok, String text);

    private static boolean answered;

    public static void show(final Activity activity, final String title, final String initial, final int max) {
        activity.runOnUiThread(() -> {
            answered = false;
            final EditText field = new EditText(activity);
            field.setSingleLine(true);
            field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            field.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            final InputFilter ascii = (src, start, end, dst, dstart, dend) -> {
                StringBuilder kept = new StringBuilder();
                for (int i = start; i < end; i++) {
                    char c = src.charAt(i);
                    if (c >= 0x20 && c < 0x7F) kept.append(c);
                }
                return kept.length() == end - start ? null : kept.toString();
            };
            field.setFilters(max > 0 ? new InputFilter[] { ascii, new InputFilter.LengthFilter(max) }
                                     : new InputFilter[] { ascii });
            field.setText(initial);
            field.setSelectAllOnFocus(true);

            final AlertDialog dialog = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(title.isEmpty() ? ("es".equals(java.util.Locale.getDefault().getLanguage()) ? "Nombre" : "Name") : title)
                    .setView(field)
                    .setPositiveButton(android.R.string.ok, (d, w) -> answer(true, field.getText().toString().trim(), initial))
                    .setNegativeButton(android.R.string.cancel, (d, w) -> answer(false, initial, initial))
                    .setOnCancelListener(d -> answer(false, initial, initial))
                    .create();
            field.setOnEditorActionListener((v, id, ev) -> {
                if (id != EditorInfo.IME_ACTION_DONE) return false;
                dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick();
                return true;
            });
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            dialog.show();
            field.requestFocus();
        });
    }

    private static void answer(boolean ok, String text, String initial) {
        if (answered) return;
        answered = true;
        if (ok && text.isEmpty()) { ok = false; text = initial; }   /* an empty name is no name */
        done(ok, text);
    }
}
