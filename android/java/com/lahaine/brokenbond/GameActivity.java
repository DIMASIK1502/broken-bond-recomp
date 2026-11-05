package com.lahaine.brokenbond;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import android.content.SharedPreferences;

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
        SharedPreferences prefs = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE);
        List<String> args = new ArrayList<>(Arrays.asList(
            "--game_data_root=" + (gameDir != null ? gameDir : ""),
            "--user_data_root=" + user.getPath(),
            "--gpu_plugin=host",
            "--renderer=native",
            "--mnk_mode",
            "--log_file=" + new File(logs, "native.log").getPath(),
            "--native_resolution_scale="
                    + prefs.getString(LauncherActivity.KEY_SCALE, LauncherActivity.DEFAULT_SCALE),
            "--native_msaa=" + prefs.getString(LauncherActivity.KEY_MSAA, LauncherActivity.DEFAULT_MSAA),
            "--native_interpolate=" + prefs.getBoolean(LauncherActivity.KEY_INTERPOLATE, false),
            "--native_frames_in_flight=" + (prefs.getBoolean(LauncherActivity.KEY_LOW_LATENCY, false) ? 1 : 2),
            "--native_record_thread=" + prefs.getBoolean(LauncherActivity.KEY_RECORD_THREAD, true),
            "--native_watch_memory=" + prefs.getBoolean(LauncherActivity.KEY_WATCH_MEMORY, true),
            "--android_big_cores=" + prefs.getBoolean(LauncherActivity.KEY_BIG_CORES, true),
            "--android_profile=" + prefs.getBoolean(LauncherActivity.KEY_PROFILE, false)));
        // A custom Vulkan driver (Turnip) from the launcher, loaded by
        // libadrenotools with its hooks from the native library folder.
        String driver = prefs.getString(LauncherActivity.KEY_DRIVER, "");
        if (!driver.isEmpty() && new File(driver).isFile()) {
            args.add("--android_vulkan_driver=" + driver);
            args.add("--android_native_lib_dir=" + getApplicationInfo().nativeLibraryDir);
        }
        return args.toArray(new String[0]);
    }
}
