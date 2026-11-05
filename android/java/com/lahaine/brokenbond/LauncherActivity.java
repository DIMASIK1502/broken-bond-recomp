package com.lahaine.brokenbond;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.json.JSONObject;

// First screen: all-files access, then the folder that holds default.xex.
// The game reads that folder in place; nothing is copied.
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final int PICK_DRIVER = 2;
    static final String PREFS = "launcher";
    private static final String KEY_GAME_DIR = "game_dir";
    // Renderer settings, passed to the game as cvars by GameActivity.
    static final String KEY_SCALE = "native_resolution_scale";
    static final String KEY_MSAA = "native_msaa";
    static final String KEY_INTERPOLATE = "native_interpolate";
    // One frame in flight: the GPU draws a frame before the next is recorded.
    static final String KEY_LOW_LATENCY = "low_latency";
    // Vulkan commands recorded on a second thread (native_record_thread).
    static final String KEY_RECORD_THREAD = "record_thread";
    // Vertex, index and texture checks skipped while the game leaves them alone
    // (native_watch_memory).
    static final String KEY_WATCH_MEMORY = "watch_memory";
    // A custom Vulkan driver (AdrenoTools package, e.g. Turnip): the .so path
    // inside the private folder, and a name to show.
    static final String KEY_DRIVER = "vulkan_driver";
    static final String KEY_DRIVER_NAME = "vulkan_driver_name";
    static final String DEFAULT_SCALE = "1";
    static final String DEFAULT_MSAA = "2";

    private static final String[] SCALE_LABELS = {
        "По размеру экрана", "360p (0.5)", "540p (0.75)", "720p (1.0)", "1080p (1.5)", "1440p (2.0)"};
    private static final String[] SCALE_VALUES = {"0", "0.5", "0.75", "1", "1.5", "2"};
    private static final String[] MSAA_LABELS = {"Выключено", "2x", "4x"};
    private static final String[] MSAA_VALUES = {"1", "2", "4"};

    private TextView status;
    private Button access;
    private Button pick;
    private Button play;
    private TextView driver;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);

        status = new TextView(this);
        status.setTextSize(18);
        status.setGravity(Gravity.CENTER);
        layout.addView(status);

        access = new Button(this);
        access.setText("Разрешить доступ ко всем файлам");
        access.setOnClickListener(v -> startActivity(new Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + getPackageName()))));
        layout.addView(access);

        pick = new Button(this);
        pick.setText("Выбрать папку с игрой");
        pick.setOnClickListener(v -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER));
        layout.addView(pick);

        play = new Button(this);
        play.setText("Играть");
        play.setOnClickListener(v -> {
            Intent intent = new Intent(this, GameActivity.class);
            intent.putExtra(GameActivity.EXTRA_GAME_DIR, gameDir());
            startActivity(intent);
        });
        layout.addView(play);

        TextView settings = new TextView(this);
        settings.setText("Графика");
        settings.setTextSize(18);
        settings.setPadding(0, pad, 0, 0);
        layout.addView(settings);
        layout.addView(choice("Внутреннее разрешение", KEY_SCALE, DEFAULT_SCALE, SCALE_LABELS, SCALE_VALUES));
        layout.addView(choice("Сглаживание (MSAA)", KEY_MSAA, DEFAULT_MSAA, MSAA_LABELS, MSAA_VALUES));
        CheckBox interpolate = new CheckBox(this);
        interpolate.setText("60 FPS (промежуточные кадры)");
        interpolate.setChecked(prefs().getBoolean(KEY_INTERPOLATE, false));
        interpolate.setOnCheckedChangeListener(
                (button, checked) -> prefs().edit().putBoolean(KEY_INTERPOLATE, checked).apply());
        layout.addView(interpolate);
        CheckBox lowLatency = new CheckBox(this);
        lowLatency.setText("Меньше задержка ввода (1 кадр в работе)");
        lowLatency.setChecked(prefs().getBoolean(KEY_LOW_LATENCY, false));
        lowLatency.setOnCheckedChangeListener(
                (button, checked) -> prefs().edit().putBoolean(KEY_LOW_LATENCY, checked).apply());
        layout.addView(lowLatency);
        CheckBox recordThread = new CheckBox(this);
        recordThread.setText("Отрисовка во втором потоке (быстрее в боях)");
        recordThread.setChecked(prefs().getBoolean(KEY_RECORD_THREAD, true));
        recordThread.setOnCheckedChangeListener(
                (button, checked) -> prefs().edit().putBoolean(KEY_RECORD_THREAD, checked).apply());
        layout.addView(recordThread);
        CheckBox watchMemory = new CheckBox(this);
        watchMemory.setText("Не пересчитывать неизменные данные (быстрее в боях)");
        watchMemory.setChecked(prefs().getBoolean(KEY_WATCH_MEMORY, true));
        watchMemory.setOnCheckedChangeListener(
                (button, checked) -> prefs().edit().putBoolean(KEY_WATCH_MEMORY, checked).apply());
        layout.addView(watchMemory);

        // libadrenotools is arm64 only (Snapdragon phones).
        if (Build.SUPPORTED_ABIS.length > 0 && Build.SUPPORTED_ABIS[0].startsWith("arm64")) {
            TextView driverTitle = new TextView(this);
            driverTitle.setText("Драйвер Vulkan");
            driverTitle.setTextSize(18);
            driverTitle.setPadding(0, pad, 0, 0);
            layout.addView(driverTitle);
            driver = new TextView(this);
            layout.addView(driver);
            Button pickDriver = new Button(this);
            pickDriver.setText("Выбрать драйвер (.zip, AdrenoTools / Turnip)");
            pickDriver.setOnClickListener(v -> {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, PICK_DRIVER);
            });
            layout.addView(pickDriver);
            Button systemDriver = new Button(this);
            systemDriver.setText("Системный драйвер");
            systemDriver.setOnClickListener(v -> {
                deleteTree(driverDir());
                prefs().edit().remove(KEY_DRIVER).remove(KEY_DRIVER_NAME).apply();
                refresh();
            });
            layout.addView(systemDriver);
        }

        ScrollView scroll = new ScrollView(this);
        scroll.addView(layout);
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private String gameDir() {
        return prefs().getString(KEY_GAME_DIR, "");
    }

    // A labelled drop-down that stores values[i] under key.
    private View choice(String title, String key, String fallback, String[] labels, String[] values) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView label = new TextView(this);
        label.setText(title);
        label.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        row.addView(label);
        Spinner spinner = new Spinner(this);
        spinner.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, labels));
        String current = prefs().getString(key, fallback);
        for (int i = 0; i < values.length; i++) {
            if (values[i].equals(current)) spinner.setSelection(i);
        }
        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                prefs().edit().putString(key, values[position]).apply();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        row.addView(spinner);
        return row;
    }

    private static boolean hasGame(String dir) {
        return !dir.isEmpty() && new File(dir, "default.xex").isFile();
    }

    private void refresh() {
        boolean granted = Environment.isExternalStorageManager();
        String dir = gameDir();
        access.setVisibility(granted ? Button.GONE : Button.VISIBLE);
        pick.setEnabled(granted);
        play.setEnabled(granted && hasGame(dir));
        if (driver != null) {
            String name = prefs().getString(KEY_DRIVER_NAME, "");
            driver.setText(name.isEmpty() ? "Сейчас: системный" : "Сейчас: " + name);
        }
        if (!granted) {
            status.setText("Нужен доступ ко всем файлам, чтобы читать папку с игрой.");
        } else if (dir.isEmpty()) {
            status.setText("Выбери папку, в которой лежит default.xex.");
        } else if (!hasGame(dir)) {
            status.setText("В папке нет default.xex:\n" + dir);
        } else {
            status.setText("Папка с игрой:\n" + dir);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PICK_DRIVER && resultCode == RESULT_OK && data != null) {
            installDriver(data.getData());
            refresh();
            return;
        }
        if (requestCode != PICK_FOLDER || resultCode != RESULT_OK || data == null) return;
        String dir = treeUriToPath(data.getData());
        if (dir != null) {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_GAME_DIR, dir).apply();
        }
        refresh();
    }

    // The driver lives in the private files folder: the linker refuses .so
    // files on shared storage.
    private File driverDir() {
        return new File(getFilesDir(), "driver");
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        file.delete();
    }

    // Unpacks an AdrenoTools driver zip. meta.json names the library
    // ("libraryName") and its version; without it the first .so is taken.
    private void installDriver(Uri uri) {
        File dir = driverDir();
        deleteTree(dir);
        dir.mkdirs();
        String library = null;
        try (InputStream in = getContentResolver().openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(in)) {
            byte[] buffer = new byte[1 << 16];
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                File out = new File(dir, entry.getName());
                if (!out.getCanonicalPath().startsWith(dir.getCanonicalPath() + File.separator)) continue;
                if (entry.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                try (OutputStream os = new FileOutputStream(out)) {
                    for (int n; (n = zip.read(buffer)) > 0; ) os.write(buffer, 0, n);
                }
                if (library == null && out.getName().endsWith(".so")) library = out.getName();
            }
            String name = library;
            File meta = new File(dir, "meta.json");
            if (meta.isFile()) {
                JSONObject json = new JSONObject(
                        new String(Files.readAllBytes(meta.toPath()), StandardCharsets.UTF_8));
                library = json.optString("libraryName", library);
                name = json.optString("name", library) + " " + json.optString("driverVersion", "");
            }
            if (library == null || !new File(dir, library).isFile()) {
                throw new IOException("в архиве нет .so драйвера");
            }
            prefs().edit()
                    .putString(KEY_DRIVER, new File(dir, library).getPath())
                    .putString(KEY_DRIVER_NAME, name.trim())
                    .apply();
        } catch (Exception e) {
            deleteTree(dir);
            prefs().edit().remove(KEY_DRIVER).remove(KEY_DRIVER_NAME).apply();
            status.setText("Драйвер не установлен: " + e.getMessage());
        }
    }

    // A tree URI of the external storage provider names "<volume>:<path>".
    // "primary" is the internal shared storage; others are SD cards and USB
    // drives mounted under /storage/<volume>.
    private static String treeUriToPath(Uri uri) {
        if (uri == null || !"com.android.externalstorage.documents".equals(uri.getAuthority())) {
            return null;
        }
        String id = DocumentsContract.getTreeDocumentId(uri);
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon);
        String path = id.substring(colon + 1);
        String root = "primary".equals(volume)
                ? Environment.getExternalStorageDirectory().getPath()
                : "/storage/" + volume;
        return path.isEmpty() ? root : root + "/" + path;
    }
}
