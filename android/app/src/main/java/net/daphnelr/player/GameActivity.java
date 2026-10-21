// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HypseusHW contributors

package net.daphnelr.player;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * Runs lr-host (libmain.so) with the bundled libretro core.  When the game
 * exits (Select+Start held, Back, or the core shuts down) the activity
 * finishes and control returns to the launcher.
 */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String[] getArguments() {
        String core = getApplicationInfo().nativeLibraryDir + "/libhypseushw_libretro_android.so";
        String rom = getIntent().getStringExtra("ROM");
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        File system = new File(base, "system");
        File saves = new File(base, "saves");
        system.mkdirs();
        saves.mkdirs();
        return new String[] { core, rom, "--system", system.getPath(), "--save", saves.getPath() };
    }
}
