package com.retroportlab.gizmondodroid;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import android.app.AlertDialog;
import android.database.Cursor;
import android.provider.OpenableColumns;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Locale;

/** GizmondoDroid front-end: a library of the Windows CE programs in your games folder, settings, and the in-game menu. */
public class MainActivity extends Activity {
    static final int BG = 0xFF0C0D14, PANEL = 0xFF171926, TEXT = 0xFFEDEEF5, SUBTEXT = 0xFF9A9DB5, ACCENT = 0xFF4FA3FF;
    static final String[] FILTERS = { "Sharp", "Smooth 3x", "Scanlines", "LCD grid" };
    static final String[] BTN_NAMES = { "D-pad up", "D-pad down", "D-pad left", "D-pad right", "A", "B", "C", "Start" };
    static final int[] DEFAULT_KEYS = { KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_R1 };
    static final int SCREEN_HOME = 0, SCREEN_SETTINGS = 1, SCREEN_CONTROLS = 2, SCREEN_GAME = 3, SCREEN_UPDATE = 4;

    private SharedPreferences prefs;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private int screen = SCREEN_HOME;
    private boolean running, menuOpen;
    private View pauseMenu;
    private TextView fpsView;
    private int keyMask, padMask, axisMask, capturing = -1;
    private TextView capturingRow;
    private volatile boolean updateBusy, updateCancel;
    private Updater.Release foundUpdate;
    private final Runnable fpsTick = new Runnable() {
        @Override public void run() {
            if (fpsView != null) { fpsView.setText(String.format(Locale.US, "%.0f FPS", NativeLib.getFps())); ui.postDelayed(this, 500); }
        }
    };

    // ----------------------------------------------------------------- setup --
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("gizmondodroid", MODE_PRIVATE);
        root = new FrameLayout(this);
        root.setBackgroundColor(BG);
        setContentView(root);
        hideSystemUi();
        showHome();
        if (prefs.getBoolean("autoUpdate", true)) ui.postDelayed(this::quietUpdateCheck, 1500);
    }

    @Override protected void onPause() { super.onPause(); if (running) NativeLib.setPaused(true); }
    @Override protected void onResume() {
        super.onResume(); hideSystemUi();
        if (running && !menuOpen && screen == SCREEN_GAME) NativeLib.setPaused(false);
        if (screen == SCREEN_HOME && !running) showHome();
    }
    @Override protected void onDestroy() { super.onDestroy(); if (running) { NativeLib.stop(); running = false; } }

    // ----------------------------------------------------------------- ui helpers --
    private int dp(int v) { return (int) (v * getResources().getDisplayMetrics().density + 0.5f); }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill); g.setCornerRadius(dp(12));
        if (stroke != 0) g.setStroke(dp(1), stroke);
        return g;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s); t.setTextSize(sp); t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private LinearLayout row(String title, String sub, String value, View.OnClickListener click) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(16), dp(12), dp(16), dp(12));
        r.setBackground(rounded(PANEL, 0));
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.addView(text(title, 16, TEXT, false));
        if (sub != null) col.addView(text(sub, 12, SUBTEXT, false));
        r.addView(col, new LinearLayout.LayoutParams(0, -2, 1));
        TextView v = text(value == null ? "" : value, 14, ACCENT, false);
        v.setTag("value");
        r.addView(v);
        if (click != null) { r.setOnClickListener(click); r.setFocusable(true); r.setClickable(true); }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = dp(8);
        r.setLayoutParams(lp);
        return r;
    }

    private void setRowValue(LinearLayout r, String v) { ((TextView) r.findViewWithTag("value")).setText(v); }

    private Button pill(String label, View.OnClickListener c) {
        Button b = new Button(this);
        b.setText(label); b.setAllCaps(false); b.setTextColor(TEXT);
        b.setBackground(rounded(0xFF26293D, 0));
        b.setOnClickListener(c);
        return b;
    }

    private void page(String title, LinearLayout list, View back) {
        screen = title.equals("Settings") ? SCREEN_SETTINGS : screen;
        root.removeAllViews();
        root.setBackgroundColor(BG);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(24), dp(16), dp(24), dp(16));
        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.addView(text(title, 24, TEXT, true), new LinearLayout.LayoutParams(0, -2, 1));
        if (back != null) head.addView(back);
        col.addView(head);
        ScrollView sv = new ScrollView(this);
        list.setPadding(0, dp(12), 0, dp(12));
        sv.addView(list);
        col.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(col, new FrameLayout.LayoutParams(-1, -1));
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }

    // ----------------------------------------------------------------- storage --
    private File gamesDir() {
        String p = prefs.getString("games", null);
        File d = p != null ? new File(p) : new File(Environment.getExternalStorageDirectory(), "GizmondoDroid");
        return d;
    }

    private boolean hasAllFiles() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void askAllFiles() {
        try {
            if (Build.VERSION.SDK_INT >= 30) {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
            } else requestPermissions(new String[]{ Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE }, 1);
        } catch (Exception e) { toast("Open Android Settings > Apps > GizmondoDroid > Permissions"); }
    }

    private File sysDir() { File d = new File(getFilesDir(), "sys"); d.mkdirs(); return d; }

    private void scan(File dir, int depth, ArrayList<File> out) {
        File[] fs = dir.listFiles();
        if (fs == null) return;
        for (File f : fs) {
            if (f.isDirectory() && depth > 0) scan(f, depth - 1, out);
            else if (f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".exe") && !f.getName().equalsIgnoreCase("autorun.exe")) out.add(f);
        }
    }

    // ----------------------------------------------------------------- home --
    private void showHome() {
        if (running) { NativeLib.stop(); running = false; }
        screen = SCREEN_HOME; menuOpen = false;
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        File dir = gamesDir();
        if (!hasAllFiles()) {
            list.addView(row("Allow access to your games folder", "GizmondoDroid needs file access to read your Gizmondo programs", "Allow", v -> askAllFiles()));
        }
        dir.mkdirs();
        ArrayList<File> exes = new ArrayList<>();
        scan(dir, 3, exes);
        Collections.sort(exes, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        if (exes.isEmpty()) {
            TextView t = text("No games yet.\nTap Install game to add a Gizmondo game (.zip or .exe) or a folder, or copy files into:\n" + dir.getAbsolutePath()
                    + "\n\nHold a game in the list to remove it.", 14, SUBTEXT, false);
            t.setPadding(0, dp(12), 0, dp(12));
            list.addView(t);
        }
        for (File f : exes) {
            String rel = f.getParentFile().equals(dir) ? "" : dir.toURI().relativize(f.getParentFile().toURI()).getPath();
            String name = f.getName().substring(0, f.getName().length() - 4);
            LinearLayout r = row(name, rel.isEmpty() ? null : rel, "Play", v -> play(f));
            r.setOnLongClickListener(v -> { confirmDelete(f); return true; });
            list.addView(r);
        }
        Button install = pill("Install game", v -> showInstallMenu());
        Button settings = pill("Settings", v -> showSettings());
        Button refresh = pill("Refresh", v -> showHome());
        LinearLayout buttons = new LinearLayout(this);
        buttons.addView(install); buttons.addView(refresh); buttons.addView(settings);
        page("GizmondoDroid", list, buttons);
    }

    // ----------------------------------------------------------------- settings --
    private int filterMode() { return prefs.getInt("filter", 0); }

    private int mappedKey(int i) { return prefs.getInt("key_" + i, DEFAULT_KEYS[i]); }

    private void showSettings() {
        screen = SCREEN_SETTINGS; menuOpen = false;
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        LinearLayout filter = row("Screen filter", null, FILTERS[filterMode()], null);
        filter.setOnClickListener(v -> { int f = (filterMode() + 1) % FILTERS.length; prefs.edit().putInt("filter", f).apply(); setRowValue(filter, FILTERS[f]); });
        list.addView(filter);
        LinearLayout stretch = row("Fill the screen", "Off keeps the original 4:3 shape", prefs.getBoolean("stretch", false) ? "On" : "Off", null);
        stretch.setOnClickListener(v -> toggle("stretch", false, stretch));
        list.addView(stretch);
        LinearLayout overlay = row("On-screen buttons", "A touch D-pad and buttons around the screen", prefs.getBoolean("overlay", true) ? "On" : "Off", null);
        overlay.setOnClickListener(v -> toggle("overlay", true, overlay));
        list.addView(overlay);
        LinearLayout fps = row("Show FPS", null, prefs.getBoolean("showFps", false) ? "On" : "Off", null);
        fps.setOnClickListener(v -> toggle("showFps", false, fps));
        list.addView(fps);
        list.addView(row("Graphics", "Frameskip, texture filtering, fog, CPU mode", null, v -> showGraphics()));
        list.addView(row("Controls", "Remap the controller buttons", null, v -> showControls()));
        list.addView(row("Game keys", "Choose which key each Gizmondo button presses in the game", null, v -> showGameKeys()));
        list.addView(row("Games folder", gamesDir().getAbsolutePath(), null, v -> toast("Put your .exe files in that folder")));
        LinearLayout au = row("Check for updates on start", null, prefs.getBoolean("autoUpdate", true) ? "On" : "Off", null);
        au.setOnClickListener(v -> toggle("autoUpdate", true, au));
        list.addView(au);
        list.addView(row("Check for updates now", "Version " + versionName(), null, v -> manualUpdateCheck()));
        list.addView(text("Save states aren't available yet in HLE mode. Original Gizmondo firmware boot is planned.", 12, SUBTEXT, false));
        page("Settings", list, pill("Back", v -> showHome()));
    }

    // ----------------------------------------------------------------- graphics & game keys --
    private static final String[] GFX_FRAMESKIP = { "Off", "Auto", "Always (30 fps)" };
    private static final String[] GFX_FILTER = { "Game's choice", "Sharp (nearest)", "Smooth (bilinear)" };
    private static final String[] GFX_PERSP = { "Best", "Balanced", "Fastest" };
    private static final String[] GFX_CPU = { "Interpreter", "JIT (fast)" };
    private static final String[] KEY_NAMES = { "Default", "None", "Enter", "Space", "Esc", "Ctrl", "Shift", "Tab", "Up", "Down", "Left", "Right", "Z", "X", "C", "A", "S", "D", "F1", "F2" };
    private static final int[] KEY_VKS = { -1, 0, 0x0D, 0x20, 0x1B, 0x11, 0x10, 0x09, 0x26, 0x28, 0x25, 0x27, 0x5A, 0x58, 0x43, 0x41, 0x53, 0x44, 0x70, 0x71 };

    private void cycleRow(LinearLayout list, String label, String sub, String key, String[] names, int def) {
        LinearLayout r = row(label, sub, names[prefs.getInt(key, def) % names.length], null);
        r.setOnClickListener(v -> { int n = (prefs.getInt(key, def) + 1) % names.length; prefs.edit().putInt(key, n).apply(); setRowValue(r, names[n]); });
        list.addView(r);
    }

    private void showGraphics() {
        screen = SCREEN_SETTINGS; menuOpen = false;
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.addView(text("These apply the next time you start a game.", 13, SUBTEXT, false));
        cycleRow(list, "Frameskip", "Auto drops frames only when the device falls behind; keeps speed and sound steady", "gfx_skip", GFX_FRAMESKIP, 1);
        cycleRow(list, "Texture filtering", "Sharp is fastest", "gfx_filter", GFX_FILTER, 0);
        LinearLayout fog = row("Fog", "Off is a little faster", prefs.getBoolean("gfx_fog", true) ? "On" : "Off", null);
        fog.setOnClickListener(v -> toggle("gfx_fog", true, fog));
        list.addView(fog);
        cycleRow(list, "Texture accuracy", "Fastest may warp textures slightly on big polygons", "gfx_persp", GFX_PERSP, 1);
        cycleRow(list, "CPU mode", "Try the interpreter if a game crashes with JIT", "gfx_cpu", GFX_CPU, 1);
        page("Graphics", list, pill("Back", v -> showSettings()));
    }

    private void showGameKeys() {
        screen = SCREEN_SETTINGS; menuOpen = false;
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.addView(text("Tap a button to cycle which keyboard key the game receives. Default uses the game's built-in profile. Applies the next time you start a game.", 13, SUBTEXT, false));
        for (int i = 0; i < 8; i++) cycleRow(list, BTN_NAMES[i], null, "gk_" + i, KEY_NAMES, 0);
        list.addView(row("Reset to defaults", null, null, v -> { SharedPreferences.Editor e = prefs.edit(); for (int i = 0; i < 8; i++) e.remove("gk_" + i); e.apply(); showGameKeys(); }));
        page("Game keys", list, pill("Back", v -> showSettings()));
    }

    private void applyGameOptions() {
        NativeLib.setOption(0, prefs.getInt("gfx_skip", 1));
        NativeLib.setOption(1, prefs.getInt("gfx_filter", 0));
        NativeLib.setOption(2, prefs.getBoolean("gfx_fog", true) ? 1 : 0);
        NativeLib.setOption(3, prefs.getInt("gfx_persp", 1));
        NativeLib.setOption(4, prefs.getInt("gfx_cpu", 1));
        for (int i = 0; i < 8; i++) { int k = prefs.getInt("gk_" + i, 0) % KEY_VKS.length; NativeLib.setAlias(i, KEY_VKS[k], 0); }
    }

    private void toggle(String key, boolean def, LinearLayout r) {
        boolean v = !prefs.getBoolean(key, def);
        prefs.edit().putBoolean(key, v).apply();
        setRowValue(r, v ? "On" : "Off");
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return "0"; }
    }

    private static String keyName(int k) { return k == 0 ? "None" : KeyEvent.keyCodeToString(k).replace("KEYCODE_", ""); }

    private void showControls() {
        screen = SCREEN_CONTROLS; capturing = -1;
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.addView(text("Tap a button, then press the key or controller button you want for it.", 13, SUBTEXT, false));
        for (int i = 0; i < 8; i++) {
            final int idx = i;
            LinearLayout[] holder = new LinearLayout[1];
            holder[0] = row(BTN_NAMES[i], null, keyName(mappedKey(i)), v -> { capturing = idx; capturingRow = (TextView) holder[0].findViewWithTag("value"); capturingRow.setText("press a key…"); });
            list.addView(holder[0]);
        }
        list.addView(row("Reset to defaults", null, null, v -> { SharedPreferences.Editor e = prefs.edit(); for (int i = 0; i < 8; i++) e.remove("key_" + i); e.apply(); showControls(); }));
        page("Controls", list, pill("Back", v -> showSettings()));
        screen = SCREEN_CONTROLS;
    }

    // ----------------------------------------------------------------- updates --
    private void quietUpdateCheck() {
        new Thread(() -> {
            try {
                Updater.Release r = Updater.latest();
                if (r != null && Updater.newer(r.tag, versionName())) ui.post(() -> { foundUpdate = r; if (screen == SCREEN_HOME) toast("Update " + r.tag + " available (Settings)"); });
            } catch (Exception ignored) { }
        }).start();
    }

    private void manualUpdateCheck() {
        toast("Checking…");
        new Thread(() -> {
            String msg; Updater.Release found = null;
            try {
                Updater.Release r = Updater.latest();
                if (r == null) msg = "No releases yet";
                else if (Updater.newer(r.tag, versionName())) { found = r; msg = r.tag + " available"; }
                else msg = "You're up to date";
            } catch (Exception e) { msg = "Couldn't reach GitHub"; }
            final String m = msg; final Updater.Release f = found;
            ui.post(() -> { toast(m); if (f != null) showUpdate(f); });
        }).start();
    }

    private void showUpdate(Updater.Release r) {
        screen = SCREEN_UPDATE; updateCancel = false;
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        LinearLayout[] dl = new LinearLayout[1];
        dl[0] = row("Download and install " + r.tag, "From GitHub releases", null, v -> downloadAndInstall(r, dl[0]));
        list.addView(dl[0]);
        list.addView(row("Open the releases page", Updater.RELEASES_PAGE, null, v -> startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(Updater.RELEASES_PAGE)))));
        page("Update", list, pill("Back", v -> { updateCancel = true; showSettings(); }));
        screen = SCREEN_UPDATE;
    }

    private void downloadAndInstall(Updater.Release r, LinearLayout rowView) {
        if (updateBusy) { updateCancel = true; return; }
        if (!Updater.canInstall(this)) {
            toast("Allow GizmondoDroid to install apps, then try again");
            try { Updater.openInstallPermission(this); } catch (Exception e) { toast("Open Android Settings > Apps > GizmondoDroid > Install unknown apps"); }
            return;
        }
        updateBusy = true; updateCancel = false;
        new Thread(() -> {
            File apk = null; String err = null;
            try {
                apk = Updater.download(this, r, (done, total) -> {
                    int pct = total > 0 ? (int) (done * 100 / total) : 0;
                    ui.post(() -> setRowValue(rowView, pct + "%  (tap to cancel)"));
                    return !updateCancel;
                });
                if (apk != null) Updater.install(this, apk);
            } catch (Exception e) { err = "Download failed"; }
            final String m = err;
            updateBusy = false;
            ui.post(() -> { setRowValue(rowView, ""); if (m != null) toast(m); });
        }).start();
    }


    // ----------------------------------------------------------------- installer --
    static final int REQ_FILE = 100, REQ_TREE = 101;
    private volatile boolean installing;

    /** The folder the game sees as \\Storage Card: its top-level folder in the library (or the library itself). */
    private File gameRoot(File exe) {
        File lib = gamesDir(), d = exe.getParentFile();
        while (d != null && !d.getParentFile().equals(lib) && !d.equals(lib)) d = d.getParentFile();
        return d == null ? lib : d;
    }

    private void showInstallMenu() {
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.addView(text("Install a Gizmondo game from a .zip (as downloaded), a single .exe, or a folder. It is copied into the library and given its own virtual SD card.", 13, SUBTEXT, false));
        list.addView(row("From a .zip or .exe file", "Pick the file", null, v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*");
            startActivityForResult(i, REQ_FILE);
        }));
        list.addView(row("From a folder", "Copies everything inside it", null, v ->
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_TREE)));
        page("Install game", list, pill("Back", v -> showHome()));
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        if (req == REQ_FILE) runInstall(() -> installFile(u));
        else if (req == REQ_TREE) runInstall(() -> installTree(u));
    }

    private void showBusy(String msg) {
        ui.post(() -> {
            View old = root.findViewWithTag("busy");
            if (old instanceof TextView) { ((TextView) old).setText(msg); return; }
            TextView t = text(msg, 16, TEXT, true);
            t.setTag("busy"); t.setGravity(Gravity.CENTER); t.setBackgroundColor(0xE0000000); t.setClickable(true);
            root.addView(t, new FrameLayout.LayoutParams(-1, -1));
        });
    }

    interface Job { void run() throws Exception; }

    private void runInstall(Job job) {
        if (installing) return;
        installing = true;
        showBusy("Installing…");
        new Thread(() -> {
            String err = null;
            try { job.run(); } catch (Exception e) { err = e.getMessage() != null ? e.getMessage() : "Install failed"; }
            final String m = err;
            installing = false;
            ui.post(() -> { toast(m == null ? "Installed" : m); showHome(); });
        }).start();
    }

    private String displayName(Uri u) {
        try (Cursor c = getContentResolver().query(u, new String[]{ OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) { }
        String p = u.getLastPathSegment(); return p == null ? "game" : p.substring(p.lastIndexOf('/') + 1);
    }

    private static String cleanName(String n) {
        int dot = n.lastIndexOf('.'); if (dot > 0) n = n.substring(0, dot);
        n = n.replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9 _.()+-]", "_").trim();
        return n.isEmpty() ? "game" : n;
    }

    private void installFile(Uri u) throws Exception {
        String name = displayName(u);
        File dest = new File(gamesDir(), cleanName(name));
        dest.mkdirs();
        if (name.toLowerCase(Locale.ROOT).endsWith(".zip")) {
            try (InputStream in = getContentResolver().openInputStream(u); ZipInputStream z = new ZipInputStream(in)) {
                ZipEntry e; int n = 0; byte[] buf = new byte[65536];
                String base = dest.getCanonicalPath() + File.separator;
                while ((e = z.getNextEntry()) != null) {
                    File out = new File(dest, e.getName());
                    if (!out.getCanonicalPath().startsWith(base)) continue;      // no path escapes
                    if (e.isDirectory()) { out.mkdirs(); continue; }
                    out.getParentFile().mkdirs();
                    try (OutputStream o = new FileOutputStream(out)) { int r; while ((r = z.read(buf)) > 0) o.write(buf, 0, r); }
                    if (++n % 20 == 0) showBusy("Installing " + dest.getName() + "…\n" + n + " files");
                }
            }
        } else if (name.toLowerCase(Locale.ROOT).endsWith(".exe")) {
            copyUri(u, new File(dest, name));
        } else throw new Exception("Pick a .zip or .exe file");
    }

    private void copyUri(Uri u, File out) throws Exception {
        out.getParentFile().mkdirs();
        try (InputStream in = getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(out)) {
            byte[] buf = new byte[65536]; int r; while ((r = in.read(buf)) > 0) o.write(buf, 0, r);
        }
    }

    private void installTree(Uri tree) throws Exception {
        String id = android.provider.DocumentsContract.getTreeDocumentId(tree);
        File dest = new File(gamesDir(), cleanName(id.substring(id.lastIndexOf(':') + 1).replace('/', ' ')));
        dest.mkdirs();
        int[] count = { 0 };
        copyTree(tree, id, dest, count);
        if (count[0] == 0) throw new Exception("That folder is empty");
    }

    private void copyTree(Uri tree, String docId, File dest, int[] count) throws Exception {
        Uri kids = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (Cursor c = getContentResolver().query(kids, new String[]{ android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME, android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE }, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String cid = c.getString(0), nm = c.getString(1).replaceAll("[\\\\/:*?\"<>|]", "_");
                if (android.provider.DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))) { File d = new File(dest, nm); d.mkdirs(); copyTree(tree, cid, d, count); }
                else { copyUri(android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, cid), new File(dest, nm)); if (++count[0] % 10 == 0) showBusy("Copying…\n" + count[0] + " files"); }
            }
        }
    }

    private void confirmDelete(File exe) {
        File root = gameRoot(exe);
        boolean wholeLibrary = root.equals(gamesDir());
        String what = wholeLibrary ? exe.getName() : root.getName();
        new AlertDialog.Builder(this).setTitle("Remove " + what + "?")
            .setMessage(wholeLibrary ? "Deletes this program file." : "Deletes the game folder, including its saves.")
            .setPositiveButton("Remove", (d, w) -> { deleteRec(wholeLibrary ? exe : root); showHome(); })
            .setNegativeButton("Cancel", null).show();
    }

    private static void deleteRec(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) deleteRec(c);
        f.delete();
    }

    // ----------------------------------------------------------------- game --
    private void play(File exe) {
        File sd = gameRoot(exe);
        applyGameOptions();
        if (!NativeLib.launch(exe.getAbsolutePath(), sd.getAbsolutePath(), sysDir().getAbsolutePath(), "")) {
            toast(NativeLib.lastError()); return;
        }
        running = true;
        enterGame();
    }

    private View btn(String label, int bit, int w, int h) {
        Button b = new Button(this);
        b.setText(label); b.setTextColor(TEXT); b.setAllCaps(false);
        b.setBackground(rounded(0x66808090, 0x66FFFFFF));
        b.setAlpha(0.7f);
        b.setOnTouchListener((v, e) -> {
            int a = e.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) padMask |= bit;
            else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) padMask &= ~bit;
            pushKeys();
            return true;
        });
        b.setLayoutParams(new FrameLayout.LayoutParams(dp(w), dp(h)));
        return b;
    }

    private void pushKeys() { NativeLib.setKeys(keyMask | padMask | axisMask); }

    private void enterGame() {
        screen = SCREEN_GAME;
        root.removeAllViews();
        root.setBackgroundColor(Color.BLACK);
        ScreenView view = new ScreenView(this, prefs.getBoolean("stretch", false));
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { NativeLib.setSurface(null); }
        });
        root.addView(view, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        keyMask = padMask = axisMask = 0; menuOpen = false; pauseMenu = null; fpsView = null;
        ui.removeCallbacks(fpsTick);
        if (prefs.getBoolean("overlay", true)) {
            // D-pad on the left, A/B/C/Start on the right
            FrameLayout pad = new FrameLayout(this);
            View up = btn("▲", NativeLib.UP, 56, 56), dn = btn("▼", NativeLib.DOWN, 56, 56), lf = btn("◀", NativeLib.LEFT, 56, 56), rt = btn("▶", NativeLib.RIGHT, 56, 56);
            place(up, 56, 0); place(dn, 56, 112); place(lf, 0, 56); place(rt, 112, 56);
            pad.addView(up); pad.addView(dn); pad.addView(lf); pad.addView(rt);
            root.addView(pad, new FrameLayout.LayoutParams(dp(168), dp(168), Gravity.BOTTOM | Gravity.START));
            FrameLayout act = new FrameLayout(this);
            View a = btn("A", NativeLib.A, 56, 56), b = btn("B", NativeLib.B, 56, 56), c = btn("C", NativeLib.C, 56, 56), s = btn("Start", NativeLib.START, 72, 40);
            place(a, 112, 56); place(b, 56, 112); place(c, 56, 0); place(s, 96, 128);
            act.addView(a); act.addView(b); act.addView(c);
            root.addView(act, new FrameLayout.LayoutParams(dp(168), dp(168), Gravity.BOTTOM | Gravity.END));
            FrameLayout.LayoutParams sp = new FrameLayout.LayoutParams(dp(72), dp(40), Gravity.TOP | Gravity.END);
            root.addView(s, sp);
            ((FrameLayout.LayoutParams) s.getLayoutParams()).setMargins(0, dp(8), dp(8), 0);
            View menu = pill("Menu", v -> openPauseMenu());
            root.addView(menu, new FrameLayout.LayoutParams(dp(72), dp(40), Gravity.TOP | Gravity.START));
        }
        if (prefs.getBoolean("showFps", false)) {
            fpsView = text("-- FPS", 14, 0xFFFFE066, true);
            fpsView.setBackgroundColor(0x80000000);
            fpsView.setPadding(dp(8), dp(2), dp(8), dp(2));
            root.addView(fpsView, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
            ui.postDelayed(fpsTick, 500);
        }
        NativeLib.setFilter(filterMode());
        NativeLib.setKeys(0);
        NativeLib.start();
        NativeLib.setPaused(false);
    }

    private void place(View v, int x, int y) {
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) v.getLayoutParams();
        lp.leftMargin = dp(x); lp.topMargin = dp(y); lp.gravity = Gravity.TOP | Gravity.START;
    }

    /** The Gizmondo screen: keeps 4:3 (unless stretched) and turns touches into stylus input. */
    static class ScreenView extends SurfaceView {
        private final boolean stretch;
        ScreenView(Context c, boolean stretch) { super(c); this.stretch = stretch; }
        @Override protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            if (!stretch) { if (w * 3 > h * 4) w = h * 4 / 3; else h = w * 3 / 4; }
            setMeasuredDimension(w, h);
        }
        @Override public boolean onTouchEvent(MotionEvent e) {
            int a = e.getActionMasked();
            int x = (int) (e.getX() * NativeLib.SCREEN_W / Math.max(1, getWidth()));
            int y = (int) (e.getY() * NativeLib.SCREEN_H / Math.max(1, getHeight()));
            x = Math.max(0, Math.min(NativeLib.SCREEN_W - 1, x));
            y = Math.max(0, Math.min(NativeLib.SCREEN_H - 1, y));
            boolean down = a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_MOVE;
            NativeLib.setTouch(x, y, down);
            return true;
        }
    }

    private void openPauseMenu() {
        if (menuOpen) return;
        menuOpen = true;
        keyMask = padMask = axisMask = 0;
        NativeLib.setKeys(0);
        NativeLib.setTouch(0, 0, false);
        NativeLib.setPaused(true);

        FrameLayout dim = new FrameLayout(this);
        dim.setBackgroundColor(0xB0000000);
        dim.setClickable(true);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(20), dp(20), dp(20));
        panel.setBackground(rounded(BG, 0x40FFFFFF));
        panel.addView(text("GizmondoDroid", 20, TEXT, true));
        TextView hint = text(NativeLib.isRunning() ? "Paused" : "The program has exited", 13, SUBTEXT, false);
        hint.setPadding(0, 0, 0, dp(12));
        panel.addView(hint);
        View resume = row("Resume", null, null, v -> closePauseMenu());
        panel.addView(resume);
        LinearLayout filter = row("Screen filter", null, FILTERS[filterMode()], null);
        filter.setOnClickListener(v -> {
            int f = (filterMode() + 1) % FILTERS.length;
            prefs.edit().putInt("filter", f).apply();
            NativeLib.setFilter(f);
            setRowValue(filter, FILTERS[f]);
        });
        panel.addView(filter);
        panel.addView(row("Quit to library", null, null, v -> showHome()));
        ScrollView sv = new ScrollView(this);
        sv.addView(panel);
        dim.addView(sv, new FrameLayout.LayoutParams(dp(420), -2, Gravity.CENTER));
        pauseMenu = dim;
        root.addView(dim, new FrameLayout.LayoutParams(-1, -1));
        resume.requestFocus();
    }

    private void closePauseMenu() {
        if (!menuOpen) return;
        if (pauseMenu != null) root.removeView(pauseMenu);
        pauseMenu = null; menuOpen = false; screen = SCREEN_GAME;
        keyMask = padMask = axisMask = 0;
        NativeLib.setKeys(0);
        if (NativeLib.isRunning()) NativeLib.setPaused(false);
    }

    // ----------------------------------------------------------------- input --
    private boolean selectHeld, startHeld;
    private int swallowUps;

    private int mapKey(int code) {
        for (int i = 0; i < 8; i++) if (mappedKey(i) == code && code != 0) return 1 << i;
        return 0;
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN, up = e.getAction() == KeyEvent.ACTION_UP;

        if (screen == SCREEN_CONTROLS && capturing >= 0) {
            if (down && e.getRepeatCount() == 0) {
                int k = code == KeyEvent.KEYCODE_BACK ? 0 : code;
                prefs.edit().putInt("key_" + capturing, k).apply();
                capturingRow.setText(keyName(k));
                capturing = -1;
            }
            return true;
        }
        if (menuOpen && swallowUps > 0 && up && (code == KeyEvent.KEYCODE_BUTTON_START || code == KeyEvent.KEYCODE_BUTTON_SELECT)) { swallowUps--; return true; }
        if (screen != SCREEN_GAME || menuOpen) {
            if (code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_DPAD_CENTER) {
                if (up) { View f = getCurrentFocus(); if (f != null) f.performClick(); }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_START) {
                if (up) {
                    if (menuOpen) closePauseMenu();
                    else if (screen == SCREEN_SETTINGS) showHome();
                    else if (screen == SCREEN_CONTROLS) showSettings();
                    else if (screen == SCREEN_UPDATE) { updateCancel = true; showSettings(); }
                    else if (code == KeyEvent.KEYCODE_BACK) finish();
                }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_SELECT) return true;
            return super.dispatchKeyEvent(e);
        }

        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) selectHeld = down;
        if (code == KeyEvent.KEYCODE_BUTTON_START) startHeld = down;
        if (selectHeld && startHeld) { selectHeld = startHeld = false; swallowUps = 2; openPauseMenu(); return true; }
        if (code == KeyEvent.KEYCODE_BACK) { if (up) openPauseMenu(); return true; }
        int bit = mapKey(code);
        if (bit == 0) return super.dispatchKeyEvent(e);
        if (down) keyMask |= bit; else if (up) keyMask &= ~bit;
        pushKeys();
        return true;
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (screen == SCREEN_GAME && !menuOpen && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && e.getAction() == MotionEvent.ACTION_MOVE) {
            float x = e.getAxisValue(MotionEvent.AXIS_X), y = e.getAxisValue(MotionEvent.AXIS_Y);
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int m = 0;
            if (hy < -0.5f || y < -0.5f) m |= NativeLib.UP;
            if (hy > 0.5f || y > 0.5f) m |= NativeLib.DOWN;
            if (hx < -0.5f || x < -0.5f) m |= NativeLib.LEFT;
            if (hx > 0.5f || x > 0.5f) m |= NativeLib.RIGHT;
            axisMask = m;
            pushKeys();
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    private void hideSystemUi() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }
}
