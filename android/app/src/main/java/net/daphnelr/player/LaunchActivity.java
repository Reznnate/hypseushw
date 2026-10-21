// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HypseusHW contributors

package net.daphnelr.player;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Toast;

/**
 * Resolves which game to run, makes sure storage can be read, then starts
 * GameActivity.  Launched directly by HyperSpin (or the home screen, which
 * re-runs the last game).
 */
public class LaunchActivity extends Activity {
    private static final int REQ_STORAGE = 1;
    private String rom;
    private boolean askedForAccess;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        rom = resolveRom(getIntent());

        if (rom != null && rom.startsWith("content:")) {
            // a document URI (file picker, share): the core opens files by path
            Toast.makeText(this, "Daphne needs a file path, not a shared document. "
                    + "Launch the game from your frontend.", Toast.LENGTH_LONG).show();
            rom = null;
            finish();
            return;
        }

        SharedPreferences prefs = getSharedPreferences("daphne", MODE_PRIVATE);
        if (rom != null) {
            prefs.edit().putString("last_rom", rom).apply();
        } else {
            rom = prefs.getString("last_rom", null);
        }

        if (rom == null) {
            Toast.makeText(this, "No game given. Launch a framefile (.txt), ROM (.zip) "
                    + "or batch file (.bat) from your frontend.", Toast.LENGTH_LONG).show();
            finish();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (rom == null || isFinishing()) return;

        if (hasStorageAccess()) {
            start();
        } else if (!askedForAccess) {
            askedForAccess = true;
            requestStorageAccess();
        } else {
            Toast.makeText(this, "Daphne needs \"All files access\" to read your games. "
                    + "On Android TV without that setting run: adb shell appops set --uid "
                    + getPackageName() + " MANAGE_EXTERNAL_STORAGE allow",
                    Toast.LENGTH_LONG).show();
            finish();
        }
    }

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    private void requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException e) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (ActivityNotFoundException e2) {
                    onResume(); // no settings screen on this device: explain and exit
                }
            }
        } else {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE },
                    REQ_STORAGE);
        }
    }

    private void start() {
        Intent game = new Intent(this, GameActivity.class);
        game.putExtra("ROM", rom);
        startActivity(game);
        finish();
    }

    static String resolveRom(Intent intent) {
        if (intent == null) return null;
        for (String key : new String[] { "ROM", "rom", "path" }) {
            String v = intent.getStringExtra(key);
            if (v != null && !v.isEmpty()) return v;
        }
        Uri data = intent.getData();
        if (data != null) {
            if ("file".equals(data.getScheme())) return data.getPath();
            return data.toString();
        }
        return null;
    }
}
