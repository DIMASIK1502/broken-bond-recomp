package com.lahaine.brokenbond;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;

// First screen: all-files access, then the folder that holds default.xex.
// The game reads that folder in place; nothing is copied.
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final String PREFS = "launcher";
    private static final String KEY_GAME_DIR = "game_dir";

    private TextView status;
    private Button access;
    private Button pick;
    private Button play;

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

        setContentView(layout);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private String gameDir() {
        return getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_GAME_DIR, "");
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
        if (requestCode != PICK_FOLDER || resultCode != RESULT_OK || data == null) return;
        String dir = treeUriToPath(data.getData());
        if (dir != null) {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_GAME_DIR, dir).apply();
        }
        refresh();
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
