package com.mariopartyrd.partyboard;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

// Downloads an update the native side found on GitHub (src/port/app_update.cpp), checks its
// SHA-256 and hands it to Android's package installer. Android then asks the player to confirm
// and only installs an APK signed with the installed app's key.
final class PartyBoardUpdater {
    private static final String TAG = "PartyBoardUpdater";
    private static final int TIMEOUT_MS = 30000;

    // Percent downloaded, -1 when unknown.
    private static volatile int sProgress = -1;
    // Set by InstallStatusReceiver when the installer gives up; read once by the native side.
    private static volatile String sInstallFailure;

    private PartyBoardUpdater() {
    }

    @SuppressWarnings("deprecation")
    static long installedVersionCode(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.P ? info.getLongVersionCode() : info.versionCode;
        } catch (PackageManager.NameNotFoundException e) {
            return 0;
        }
    }

    static int progress() {
        return sProgress;
    }

    static String takeInstallFailure() {
        String failure = sInstallFailure;
        sInstallFailure = null;
        return failure;
    }

    static void reportInstallFailure(String failure) {
        Log.w(TAG, "Install failed: " + failure);
        sInstallFailure = failure;
    }

    // Blocks: the native side calls it from a worker thread. Returns null once Android's installer
    // has the APK, "checksum" when the download is not the announced file, or why it failed.
    static String downloadAndInstall(Context context, String url, String sha256) {
        sProgress = 0;
        sInstallFailure = null;
        File directory = new File(context.getCacheDir(), "updates");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            return "cannot create " + directory;
        }
        File apk = new File(directory, "partyboard-update.apk");
        try {
            String error = download(url, apk, sha256);
            if (error != null) {
                return error;
            }
            commit(context, apk);
            return null;
        } catch (IOException | NoSuchAlgorithmException | RuntimeException e) {
            Log.w(TAG, "Update failed", e);
            String message = e.getMessage();
            return message != null && !message.isEmpty() ? message : e.getClass().getSimpleName();
        } finally {
            // The installer session keeps its own copy.
            if (apk.exists() && !apk.delete()) {
                Log.w(TAG, "Could not delete " + apk);
            }
        }
    }

    private static String download(String url, File target, String expectedSha256)
            throws IOException, NoSuchAlgorithmException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(TIMEOUT_MS);
        connection.setReadTimeout(TIMEOUT_MS);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("User-Agent", "PartyBoard-Updater/1");
        try {
            int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                return "HTTP " + code;
            }
            long total = connection.getContentLengthLong();
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long received = 0;
            try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                    digest.update(buffer, 0, read);
                    received += read;
                    if (total > 0) {
                        sProgress = (int) Math.min(99, received * 100 / total);
                    }
                }
            }
            if (!hex(digest.digest()).equalsIgnoreCase(expectedSha256)) {
                return "checksum";
            }
            sProgress = 100;
            return null;
        } finally {
            connection.disconnect();
        }
    }

    private static void commit(Context context, File apk) throws IOException {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
            new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(context.getPackageName());
        params.setSize(apk.length());
        int sessionId = installer.createSession(params);
        boolean committed = false;
        try (PackageInstaller.Session session = installer.openSession(sessionId)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("partyboard.apk", 0, apk.length())) {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
                session.fsync(out);
            }
            // Explicit and mutable: the installer adds the status (and its confirmation screen) to it.
            Intent intent = new Intent(context, InstallStatusReceiver.class);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                flags |= PendingIntent.FLAG_MUTABLE;
            }
            PendingIntent pending = PendingIntent.getBroadcast(context, sessionId, intent, flags);
            session.commit(pending.getIntentSender());
            committed = true;
        } finally {
            if (!committed) {
                try {
                    installer.abandonSession(sessionId);
                } catch (RuntimeException e) {
                    Log.w(TAG, "Could not abandon install session " + sessionId, e);
                }
            }
        }
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            text.append(Character.forDigit((b >> 4) & 0xF, 16));
            text.append(Character.forDigit(b & 0xF, 16));
        }
        return text.toString();
    }
}
