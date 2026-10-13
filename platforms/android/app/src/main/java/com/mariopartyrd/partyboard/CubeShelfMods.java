package com.mariopartyrd.partyboard;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * GameBanana mods installed by CubeShelf for Android (CubeShelf.Android/PhoneMods.cs).
 *
 * CubeShelf keeps them in its own folder, which this app cannot read. When it opens the game it
 * says which mods are on, highest priority first, each with a version (its archive's SHA-256),
 * and lets this app read them through its provider, for this launch. What changed is copied here
 * (files/cubeshelf-mods/<id>), what is no longer on is removed, and the list is handed to the game
 * the way the PC launcher does: PARTYBOARD_MOD_LIST, one folder per line (src/port/mods.cpp).
 *
 * Opened from the home screen rather than by CubeShelf, the game keeps the mods CubeShelf last
 * gave it.
 */
final class CubeShelfMods {
    private static final String TAG = "CubeShelfMods";
    static final String EXTRA_MODS = "cubeshelf_mods";
    static final String EXTRA_MODS_URI = "cubeshelf_mods_uri";
    private static final String AUTHORITY = "com.zeranemesis.cubeshelf.mods";
    private static final int MAX_MODS = 64;
    private static final int MAX_FILES = 65536;
    private static final long MAX_BYTES = 4L * 1024 * 1024 * 1024;

    private CubeShelfMods() {
    }

    private static File root(Context context) {
        return new File(context.getFilesDir(), "cubeshelf-mods");
    }

    /** Brings the mods in line with what CubeShelf says, then points the game at them. */
    static void apply(Context context, Intent intent) {
        File root = root(context);
        File list = new File(root, "active-mods.txt");
        String json = intent == null ? null : intent.getStringExtra(EXTRA_MODS);
        String base = intent == null ? null : intent.getStringExtra(EXTRA_MODS_URI);
        if (json != null && base != null) {
            try {
                sync(context, root, list, new JSONArray(json), Uri.parse(base));
            } catch (JSONException | SecurityException | IOException e) {
                // What was there stays: better the last mods than a half-copied set.
                Log.w(TAG, "Mods from CubeShelf not applied", e);
            }
        }
        try {
            if (list.isFile()) {
                Os.setenv("PARTYBOARD_MOD_LIST", list.getAbsolutePath(), true);
            } else {
                Os.unsetenv("PARTYBOARD_MOD_LIST");
            }
        } catch (ErrnoException e) {
            Log.w(TAG, "Unable to pass the mod list to the game", e);
        }
    }

    private static void sync(Context context, File root, File list, JSONArray mods, Uri base) throws JSONException, IOException {
        if (!"content".equals(base.getScheme()) || !AUTHORITY.equals(base.getAuthority())) {
            throw new IOException("Not CubeShelf's mods");
        }
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create " + root);

        List<File> active = new ArrayList<>();
        Set<String> keep = new HashSet<>();
        int count = Math.min(mods.length(), MAX_MODS);
        for (int i = 0; i < count; ++i) {
            JSONObject mod = mods.getJSONObject(i);
            int id = mod.getInt("id");
            String version = mod.getString("version");
            if (id <= 0 || !version.matches("[A-Za-z0-9]{1,128}")) continue;
            File folder = new File(root, Integer.toString(id));
            File stamp = new File(folder, ".cubeshelf-version");
            if (!version.equals(readText(stamp))) {
                copy(context.getContentResolver(), base.buildUpon().appendPath(Integer.toString(id)).appendPath(version).build(), root, folder, stamp, version);
            }
            keep.add(folder.getName());
            active.add(folder);
        }

        // Mods no longer on (or removed in CubeShelf) leave this app too.
        File[] present = root.listFiles();
        if (present != null) {
            for (File entry : present) {
                if (entry.isDirectory() && !keep.contains(entry.getName())) deleteTree(entry);
            }
        }

        StringBuilder text = new StringBuilder();
        for (File folder : active) text.append(folder.getAbsolutePath()).append('\n');
        File temporary = new File(root, "active-mods.txt.tmp");
        try (OutputStream out = new FileOutputStream(temporary)) {
            out.write(text.toString().getBytes(StandardCharsets.UTF_8));
        }
        if (!temporary.renameTo(list)) throw new IOException("Cannot write " + list);
        Log.i(TAG, active.size() + " mod(s) from CubeShelf");
    }

    /** One mod, copied whole into a new folder, then put in place of the old one. */
    private static void copy(ContentResolver resolver, Uri mod, File root, File folder, File stamp, String version) throws IOException {
        File staging = new File(root, folder.getName() + ".incoming");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("Cannot create " + staging);
        String canonical = staging.getCanonicalPath() + File.separator;
        long total = 0;
        int files = 0;
        try (Cursor cursor = resolver.query(mod, new String[] {"path", "size"}, null, null, null)) {
            if (cursor == null) throw new IOException("CubeShelf did not list mod " + folder.getName());
            while (cursor.moveToNext()) {
                String path = cursor.getString(0);
                if (++files > MAX_FILES) throw new IOException("Too many files");
                if (path == null || path.isEmpty() || path.startsWith("/") || path.contains("\\")) throw new IOException("Bad path");
                for (String part : path.split("/")) {
                    if (part.isEmpty() || part.equals(".") || part.equals("..")) throw new IOException("Bad path " + path);
                }
                File target = new File(staging, path);
                if (!target.getCanonicalPath().startsWith(canonical)) throw new IOException("Bad path " + path);
                File parent = target.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create " + parent);
                Uri.Builder file = mod.buildUpon().appendPath("f");
                for (String part : path.split("/")) file.appendPath(part);
                try (InputStream in = resolver.openInputStream(file.build()); OutputStream out = new FileOutputStream(target)) {
                    if (in == null) throw new IOException("CubeShelf did not open " + path);
                    byte[] buffer = new byte[256 * 1024];
                    int read;
                    while ((read = in.read(buffer)) > 0) {
                        total += read;
                        if (total > MAX_BYTES) throw new IOException("Mod too large");
                        out.write(buffer, 0, read);
                    }
                }
            }
        } catch (IOException | RuntimeException e) {
            deleteTree(staging);
            throw e instanceof IOException ? (IOException) e : new IOException(e);
        }
        try (OutputStream out = new FileOutputStream(new File(staging, stamp.getName()))) {
            out.write(version.getBytes(StandardCharsets.UTF_8));
        }
        deleteTree(folder);
        if (!staging.renameTo(folder)) throw new IOException("Cannot put mod " + folder.getName() + " in place");
        Log.i(TAG, "Mod " + folder.getName() + ": " + files + " file(s), " + total + " bytes");
    }

    private static String readText(File file) {
        if (!file.isFile() || file.length() > 256) return null;
        try {
            return new String(java.nio.file.Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return null;
        }
    }

    private static void deleteTree(File file) {
        if (!file.exists()) return;
        File[] children = file.isDirectory() ? file.listFiles() : null;
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        if (!file.delete()) Log.w(TAG, "Could not delete " + file);
    }
}
