package com.lahaine.brokenbond;

import java.io.File;

import org.libsdl.app.SDLActivity;

// Runs the game. SDL loads the libraries below and calls SDL_main in
// libmain.so with getArguments() as argv, the same flags run_native.bat uses.
public class GameActivity extends SDLActivity {
    public static final String EXTRA_GAME_DIR = "game_dir";

    @Override
    protected String[] getLibraries() {
        // librexruntimerd.so carries SDL3 and its JNI_OnLoad, so it is loaded
        // through System.loadLibrary before libmain.so.
        return new String[] {"rexruntimerd", "main"};
    }

    @Override
    protected String[] getArguments() {
        String gameDir = getIntent().getStringExtra(EXTRA_GAME_DIR);
        // Android/data/<package>/files: saves, cache and logs, readable over adb.
        File user = getExternalFilesDir(null);
        File logs = new File(user, "logs");
        logs.mkdirs();
        return new String[] {
            "--game_data_root=" + (gameDir != null ? gameDir : ""),
            "--user_data_root=" + user.getPath(),
            "--gpu_plugin=host",
            "--renderer=native",
            "--mnk_mode",
            "--log_file=" + new File(logs, "native.log").getPath(),
        };
    }
}
