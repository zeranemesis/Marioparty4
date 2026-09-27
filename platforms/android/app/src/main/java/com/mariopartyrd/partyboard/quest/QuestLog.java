package com.mariopartyrd.partyboard.quest;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Process;
import android.provider.MediaStore;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Meta Quest: keeps the game's log in a file the player can reach without a
 * cable, in the headset's Download/PartyBoard folder (the Files app shows it,
 * and so does a PC over USB later).
 *
 * The app's own log (logcat for this process: no permission needed) is copied
 * there while the game runs: the headset's performance every 5 seconds
 * (perf_metrics.cpp: "Perf:", "Perf counters:", "Stereo perf:", "Game perf:"),
 * the scenes, placement, updates and errors. One file per launch; the last
 * five are kept.
 */
final class QuestLog {
    private static final String TAG = "PartyBoardQuest";
    private static final String FOLDER = "PartyBoard";
    private static final String PREFIX = "partyboard-quest-";
    private static final int KEEP = 5;
    private static final long MAX_BYTES = 32L * 1024 * 1024;
    private static final long FLUSH_EVERY_MS = 2000;

    private static boolean sStarted;

    private QuestLog() {
    }

    static synchronized void start(Context context) {
        if (sStarted) {
            return;
        }
        sStarted = true;
        Context app = context.getApplicationContext();
        Thread thread = new Thread(() -> run(app), "QuestLog");
        thread.setDaemon(true);
        thread.setPriority(Thread.MIN_PRIORITY);
        thread.start();
    }

    private static void run(Context context) {
        String name = PREFIX + new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(new Date()) + ".txt";
        java.lang.Process logcat = null;
        try (OutputStream stream = open(context, name);
             Writer out = new OutputStreamWriter(stream, StandardCharsets.UTF_8)) {
            out.write(header(context));
            out.flush();
            // The whole log of this process so far, then everything that follows.
            logcat = new ProcessBuilder("logcat", "-v", "threadtime", "--pid=" + Process.myPid())
                .redirectErrorStream(true).start();
            BufferedReader in = new BufferedReader(
                new InputStreamReader(logcat.getInputStream(), StandardCharsets.UTF_8));
            long written = 0;
            long flushedAt = System.currentTimeMillis();
            for (String line; (line = in.readLine()) != null; ) {
                out.write(line);
                out.write('\n');
                written += line.length() + 1;
                long now = System.currentTimeMillis();
                if (now - flushedAt >= FLUSH_EVERY_MS) {
                    out.flush();
                    flushedAt = now;
                }
                if (written >= MAX_BYTES) {
                    out.write("--- log truncated at " + MAX_BYTES + " bytes ---\n");
                    break;
                }
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Log file unavailable", e);
        } finally {
            if (logcat != null) {
                logcat.destroy();
            }
        }
    }

    private static String header(Context context) {
        String version = "?";
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            long code = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P ? info.getLongVersionCode() : legacyVersionCode(info);
            version = info.versionName + " (" + code + ")";
        } catch (Exception ignored) {
            // the version stays unknown
        }
        return "Party Board " + version + " on " + Build.MANUFACTURER + " " + Build.MODEL
            + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), "
            + new Date() + "\n";
    }

    @SuppressWarnings("deprecation")
    private static long legacyVersionCode(PackageInfo info) {
        return info.versionCode;
    }

    // Download/PartyBoard through MediaStore (Android 10+: no permission for the
    // app's own files); the app's external files folder before that.
    private static OutputStream open(Context context, String name) throws IOException {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            File dir = new File(context.getExternalFilesDir(null), "logs");
            if (!dir.isDirectory() && !dir.mkdirs()) {
                throw new IOException("Cannot create " + dir);
            }
            return new FileOutputStream(new File(dir, name));
        }
        ContentResolver resolver = context.getContentResolver();
        prune(resolver);
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        values.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/" + FOLDER);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        OutputStream stream = uri != null ? resolver.openOutputStream(uri, "w") : null;
        if (stream == null) {
            throw new IOException("Cannot create Download/" + FOLDER + "/" + name);
        }
        Log.i(TAG, "Log file: Download/" + FOLDER + "/" + name);
        return stream;
    }

    // Keeps the newest KEEP - 1 earlier logs (the new one makes KEEP).
    private static void prune(ContentResolver resolver) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return;
        }
        Uri collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        List<Uri> logs = new ArrayList<>();
        String[] projection = {MediaStore.MediaColumns._ID};
        String selection = MediaStore.MediaColumns.RELATIVE_PATH + "=? AND "
            + MediaStore.MediaColumns.DISPLAY_NAME + " LIKE ?";
        String[] args = {Environment.DIRECTORY_DOWNLOADS + "/" + FOLDER + "/", PREFIX + "%"};
        try (Cursor cursor = resolver.query(collection, projection, selection, args,
                MediaStore.MediaColumns.DISPLAY_NAME + " DESC")) {
            while (cursor != null && cursor.moveToNext()) {
                logs.add(Uri.withAppendedPath(collection, Long.toString(cursor.getLong(0))));
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Old logs not listed", e);
            return;
        }
        for (int i = KEEP - 1; i < logs.size(); ++i) {
            try {
                resolver.delete(logs.get(i), null, null);
            } catch (RuntimeException e) {
                Log.w(TAG, "Old log not removed", e);
            }
        }
    }
}
