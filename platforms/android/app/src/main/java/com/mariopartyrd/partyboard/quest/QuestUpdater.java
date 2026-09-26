package com.mariopartyrd.partyboard.quest;

import android.Manifest;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.util.Log;

import org.json.JSONObject;
import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;

/**
 * Meta Quest: installs Party Board's updates inside the headset.
 *
 * The game checks GitHub for a newer build as on a phone (src/port/app_update.cpp)
 * and, on Update, opens the phone APK's link. On a phone the browser downloads
 * it and Android installs it. A headset has no installer to open a downloaded
 * APK, so this build (the "quest" flavor, which may ask for the install
 * permission: no Play Protect here) takes the link instead: it downloads the
 * Quest APK named by quest-update.json, checks its SHA-256 and hands it to
 * Android's package installer, which asks the player to confirm and installs
 * it over this one, keeping the saves. The first install is done from a PC
 * (scripts/install-quest.cmd).
 */
public final class QuestUpdater implements SDLActivity.UrlHandler {
    private static final String TAG = "PartyBoardQuest";
    private static final String RELEASE =
        "https://github.com/zeranemesis/Marioparty4/releases/download/partyboard-android-latest/";
    private static final String MANIFEST = RELEASE + "quest-update.json";
    private static final String ACTION_STATUS = "com.mariopartyrd.partyboard.QUEST_INSTALL_STATUS";
    private static final int TIMEOUT_MS = 20_000;

    private final Activity mActivity;
    private volatile boolean mBusy;

    private QuestUpdater(Activity activity) {
        mActivity = activity;
    }

    /** Takes over the game's update links when this build may install packages. */
    public static void install(Activity activity) {
        if (!declaresInstallPermission(activity)) {
            Log.i(TAG, "Phone build: updates go through the browser");
            return;
        }
        QuestUpdater updater = new QuestUpdater(activity);
        updater.registerStatusReceiver();
        SDLActivity.mUrlHandler = updater;
    }

    private static boolean declaresInstallPermission(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(
                context.getPackageName(), PackageManager.GET_PERMISSIONS);
            return info.requestedPermissions != null
                && Arrays.asList(info.requestedPermissions).contains(Manifest.permission.REQUEST_INSTALL_PACKAGES);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    @Override
    public boolean openURL(String url) {
        if (url == null || !url.startsWith(RELEASE) || !url.endsWith(".apk")) {
            return false;
        }
        if (!mBusy) {
            mBusy = true;
            new Thread(() -> {
                try {
                    update(url);
                } finally {
                    mBusy = false;
                }
            }, "QuestUpdater").start();
        }
        return true;
    }

    private void update(String phoneUrl) {
        try {
            JSONObject manifest = new JSONObject(new String(fetch(MANIFEST), StandardCharsets.UTF_8));
            String apkUrl = manifest.getString("downloadUrl");
            String sha256 = manifest.getString("sha256");
            if (!apkUrl.startsWith(RELEASE)) {
                throw new IOException("The manifest names another site: " + apkUrl);
            }
            File apk = new File(mActivity.getCacheDir(), "quest-update.apk");
            String actual = download(apkUrl, apk);
            if (!actual.equalsIgnoreCase(sha256)) {
                apk.delete();
                throw new IOException("SHA-256 mismatch: expected " + sha256 + ", got " + actual);
            }
            Log.i(TAG, "Update downloaded (" + apk.length() + " bytes), installing");
            commit(apk);
        } catch (Exception e) {
            // The phone's way still works from a PC or a phone: the link in the browser.
            Log.e(TAG, "In-headset update failed, opening the link instead", e);
            openInBrowser(phoneUrl);
        }
    }

    private static HttpURLConnection connect(String url) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(TIMEOUT_MS);
        connection.setReadTimeout(TIMEOUT_MS);
        connection.setInstanceFollowRedirects(true); // GitHub serves assets through a redirect
        connection.setRequestProperty("User-Agent", "PartyBoard-Updater/1");
        if (connection.getResponseCode() != HttpURLConnection.HTTP_OK) {
            throw new IOException(url + ": HTTP " + connection.getResponseCode());
        }
        return connection;
    }

    private static byte[] fetch(String url) throws IOException {
        HttpURLConnection connection = connect(url);
        try (InputStream in = connection.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16 * 1024];
            for (int read; (read = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, read);
            }
            return out.toByteArray();
        } finally {
            connection.disconnect();
        }
    }

    // Downloads to `file`; returns the file's SHA-256 (hex).
    private static String download(String url, File file) throws IOException, NoSuchAlgorithmException {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        HttpURLConnection connection = connect(url);
        try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(file)) {
            byte[] buffer = new byte[256 * 1024];
            for (int read; (read = in.read(buffer)) > 0; ) {
                digest.update(buffer, 0, read);
                out.write(buffer, 0, read);
            }
        } finally {
            connection.disconnect();
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) {
            hex.append(String.format("%02x", b));
        }
        return hex.toString();
    }

    private void commit(File apk) throws IOException {
        PackageInstaller installer = mActivity.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
            new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(mActivity.getPackageName());
        params.setSize(apk.length());
        int sessionId = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(sessionId)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("partyboard.apk", 0, apk.length())) {
                byte[] buffer = new byte[256 * 1024];
                for (int read; (read = in.read(buffer)) > 0; ) {
                    out.write(buffer, 0, read);
                }
                session.fsync(out);
            }
            Intent status = new Intent(ACTION_STATUS).setPackage(mActivity.getPackageName());
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                flags |= PendingIntent.FLAG_MUTABLE; // the installer adds the status extras
            }
            PendingIntent pending = PendingIntent.getBroadcast(mActivity, sessionId, status, flags);
            session.commit(pending.getIntentSender());
        }
    }

    // The installer's answers: it first asks for the player's confirmation
    // (and, the first time, to allow Party Board to install apps).
    private void registerStatusReceiver() {
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    @SuppressWarnings("deprecation")
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (confirm != null) {
                        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        mActivity.startActivity(confirm);
                    }
                } else if (status != PackageInstaller.STATUS_SUCCESS) {
                    Log.e(TAG, "Update not installed (" + status + "): "
                        + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                }
            }
        };
        IntentFilter filter = new IntentFilter(ACTION_STATUS);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            mActivity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            mActivity.registerReceiver(receiver, filter);
        }
    }

    private void openInBrowser(String url) {
        try {
            Intent view = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            view.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            mActivity.startActivity(view);
        } catch (RuntimeException e) {
            Log.e(TAG, "Unable to open " + url, e);
        }
    }
}
