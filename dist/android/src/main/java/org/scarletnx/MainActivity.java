// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Insets;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.DisplayCutout;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** The menu: scarletnx-gui, as libmain.so */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // dumps and keys are read by their path: all files from Android 11, the shared storage before it
        if (Build.VERSION.SDK_INT >= 30) {
            if (!Environment.isExternalStorageManager())
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
        } else if (Build.VERSION.SDK_INT >= 23 &&
                   checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE}, 0);
    }

    /** src/gui/android.c: the runtime with these arguments, in the game activity's process */
    public void play(String[] args) {
        startActivity(new Intent(this, GameActivity.class).putExtra("args", args));
    }

    /** src/gui/android.c: Back on the menu's first screen, as an app's first screen takes it: the app goes behind */
    public void toBack() {
        moveTaskToBack(true);
    }

    /** src/gui/android.c: the saved log, as a file, to the app the user picks */
    public void share(String path) {
        MediaScannerConnection.scanFile(this, new String[] {path}, new String[] {"text/plain"}, (file, uri) -> {
            if (uri == null) return;
            Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_STREAM, uri);
            startActivity(Intent.createChooser(send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), "ScarletNX log"));
        });
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return clearOfBars(context);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideBars(mSurface);
    }

    /** Fullscreen on, the system's bars hidden through the window's insets controller. SDL hides them again only by
        setting its flags, which does nothing while those are still set (View.setSystemUiVisibility): bars the system
        showed meanwhile, over another window or from a swipe, would stay */
    static void hideBars(View view) {
        WindowInsetsController bars =
            Build.VERSION.SDK_INT >= 30 && view != null ? view.getWindowInsetsController() : null;
        if (mFullscreenModeActive && bars != null) bars.hide(WindowInsets.Type.systemBars());
    }

    /** a surface whose safe area keeps out of the camera's cutout and of the system's bars while they show (Fullscreen
        off), not of the bars it hides nor of the gesture edges: the menu's text and a game's picture stay there */
    static SDLSurface clearOfBars(Context context) {
        return new SDLSurface(context) {
            final Runnable hide = () -> hideBars(this);

            @Override
            public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                WindowInsets result = super.onApplyWindowInsets(view, insets);
                if (Build.VERSION.SDK_INT >= 30) {
                    Insets safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                    SDLActivity.onNativeInsetsChanged(safe.left, safe.right, safe.top, safe.bottom);
                    // bars that show hide again as late as SDL's own retry would: time to reach their buttons
                    removeCallbacks(hide);
                    if (insets.isVisible(WindowInsets.Type.statusBars()) ||
                        insets.isVisible(WindowInsets.Type.navigationBars()))
                        postDelayed(hide, 2000);
                } else {
                    DisplayCutout cut = Build.VERSION.SDK_INT >= 28 ? insets.getDisplayCutout() : null;
                    SDLActivity.onNativeInsetsChanged(
                        Math.max(insets.getSystemWindowInsetLeft(), cut != null ? cut.getSafeInsetLeft() : 0),
                        Math.max(insets.getSystemWindowInsetRight(), cut != null ? cut.getSafeInsetRight() : 0),
                        Math.max(insets.getSystemWindowInsetTop(), cut != null ? cut.getSafeInsetTop() : 0),
                        Math.max(insets.getSystemWindowInsetBottom(), cut != null ? cut.getSafeInsetBottom() : 0));
                }
                return result;
            }
        };
    }
}
