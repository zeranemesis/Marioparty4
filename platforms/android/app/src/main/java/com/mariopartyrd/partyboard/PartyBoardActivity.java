package com.mariopartyrd.partyboard;

import android.app.ActionBar;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import com.google.mlkit.vision.barcode.common.Barcode;
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions;
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning;
import com.mariopartyrd.partyboard.online.LobbyActivity;
import com.mariopartyrd.partyboard.online.RestartActivity;
import com.mariopartyrd.partyboard.online.OnlineService;
import com.mariopartyrd.partyboard.quest.QuestVr;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class PartyBoardActivity extends SDLActivity {
    private static final String TAG = "PartyBoardActivity";

    private static String[] splitArgs(String raw) {
        List<String> out = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        boolean inSingle = false;
        boolean inDouble = false;
        boolean escaped = false;

        for (int i = 0; i < raw.length(); ++i) {
            char c = raw.charAt(i);
            if (escaped) {
                current.append(c);
                escaped = false;
                continue;
            }
            if (c == '\\' && !inSingle) {
                escaped = true;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                continue;
            }
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                continue;
            }
            if (!inSingle && !inDouble && Character.isWhitespace(c)) {
                if (current.length() > 0) {
                    out.add(current.toString());
                    current.setLength(0);
                }
                continue;
            }
            current.append(c);
        }

        if (escaped) {
            current.append('\\');
        }
        if (current.length() > 0) {
            out.add(current.toString());
        }
        return out.toArray(new String[0]);
    }

    // Set when the online lobby (the ":online" process) started this game:
    // the disc it verified and the loopback barrier it waits on. Environment
    // variables because the native side reads them exactly where Windows reads
    // the ones PartyBoardOnline.exe sets (src/port/portmain.cpp).
    private boolean onlineSession;
    private String onlineLanguage;

    private void exportOnlineEnvironment(Intent intent) {
        String barrier = intent == null ? null : intent.getStringExtra(OnlineService.EXTRA_BARRIER);
        String disc = intent == null ? null : intent.getStringExtra(OnlineService.EXTRA_DISC);
        onlineSession = barrier != null && disc != null;
        onlineLanguage = intent == null ? null : intent.getStringExtra(OnlineService.EXTRA_LANGUAGE);
        try {
            if (onlineSession) {
                Os.setenv("PARTYBOARD_ONLINE_BARRIER", barrier, true);
                Os.setenv("PARTYBOARD_ONLINE_DISC", disc, true);
            } else {
                Os.unsetenv("PARTYBOARD_ONLINE_BARRIER");
                Os.unsetenv("PARTYBOARD_ONLINE_DISC");
            }
            // Opt-in private audio diagnostics for device validation.
            if (intent != null && intent.getBooleanExtra("partyboard_audio_diagnostics", false)
                    && (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
                Os.setenv("PARTYBOARD_AUDIO_DIAGNOSTIC_DIR", getFilesDir().getAbsolutePath(), true);
                Os.setenv("PARTYBOARD_AUDIO_TIMING_DIAGNOSTICS", new java.io.File(getFilesDir(), "audio_timing_diagnostic.log").getAbsolutePath(), true);
                Os.setenv("PARTYBOARD_RENDER_DIAGNOSTICS", "1", true);
                Os.setenv("PARTYBOARD_SEQ_DIAGNOSTICS", "all", true);
            }
            // Automated tests only (src/port/test_input.cpp): never in a release build.
            String testInput = intent == null ? null : intent.getStringExtra("partyboard_test_input");
            if (testInput != null && (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
                Os.setenv("PARTYBOARD_TEST_INPUT", testInput, true);
            }
        } catch (ErrnoException e) {
            Log.w(TAG, "Unable to pass the online session to the game", e);
        }
    }

    // ---- CubeShelf QR code (CubeShelfLink.java, src/port/ui/cubeshelf.cpp) -------------------
    private final Object linkLock = new Object();
    private String linkState = "idle";
    private String linkMessage = "";
    private String linkProfile = null;
    private int linkSaves = 0;
    private int linkMode = 0;

    private void setLink(String state, String message) {
        synchronized (linkLock) {
            linkState = state;
            linkMessage = message == null ? "" : message;
        }
    }

    // Called from the game (mode 0: get the account and the PC's saves, 1: send this phone's saves).
    // An empty code opens Play services' QR scanner; a pasted CSL1: code is used as it is.
    public boolean startCubeShelfLink(final int mode, final String code, final boolean french) {
        synchronized (linkLock) {
            if (linkState.equals("scanning") || linkState.equals("working")) {
                return false;
            }
            linkMode = mode;
            linkProfile = null;
            linkSaves = 0;
        }
        if (code != null && code.contains(CubeShelfLink.PREFIX)) {
            runCubeShelfLink(mode, code, french);
            return true;
        }
        setLink("scanning", "");
        runOnUiThread(() -> {
            try {
                GmsBarcodeScanning.getClient(this, new GmsBarcodeScannerOptions.Builder()
                        .setBarcodeFormats(Barcode.FORMAT_QR_CODE).build())
                    .startScan()
                    .addOnSuccessListener(barcode -> runCubeShelfLink(mode, barcode.getRawValue(), french))
                    .addOnCanceledListener(() -> setLink("error", french ? "Scan annulé." : "Scan cancelled."))
                    .addOnFailureListener(e -> setLink("error", french
                        ? "Scanner indisponible (services Google Play). Copie le code affiché sous le QR code dans CubeShelf, puis colle-le ici."
                        : "Scanner unavailable (Google Play services). Copy the code shown under the QR code in CubeShelf, then paste it here."));
            } catch (RuntimeException e) {
                setLink("error", french ? "Scanner indisponible sur cet appareil." : "Scanner unavailable on this device.");
            }
        });
        return true;
    }

    private void runCubeShelfLink(final int mode, final String code, final boolean french) {
        setLink("working", "");
        Thread worker = new Thread(() -> {
            try {
                CubeShelfLink link = CubeShelfLink.parse(code, french);
                if (mode == 0) {
                    CubeShelfLink.Received received = link.fetch(getFilesDir(), french);
                    synchronized (linkLock) {
                        linkProfile = received.profileJson;
                        linkSaves = received.saves;
                    }
                    setLink("done", "");
                } else {
                    setLink("done", link.send(getFilesDir(), french));
                }
            } catch (java.io.IOException e) {
                setLink("error", e.getMessage());
            }
        }, "CubeShelf link");
        worker.setDaemon(true);
        worker.start();
    }

    // Polled by the game each frame while its Friends tab is open. A finished result is handed
    // over once: {"state","mode","message","profile","saves"}.
    public String pollCubeShelfLink() {
        synchronized (linkLock) {
            try {
                org.json.JSONObject out = new org.json.JSONObject();
                out.put("state", linkState);
                out.put("mode", linkMode);
                out.put("message", linkMessage);
                out.put("saves", linkSaves);
                if (linkProfile != null) {
                    out.put("profile", linkProfile);
                }
                if (linkState.equals("done") || linkState.equals("error")) {
                    linkState = "idle";
                    linkProfile = null;
                }
                return out.toString();
            } catch (org.json.JSONException e) {
                return "{\"state\":\"idle\"}";
            }
        }
    }

    // Starts the game again, for saves that wait for the next start. The game ends itself right
    // after; RestartActivity (another process) opens it again once this one is gone.
    public void restartGame() {
        Intent restart = new Intent(this, RestartActivity.class);
        restart.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(restart);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Saves received from CubeShelf go in before the game opens its memory cards.
        int applied = CubeShelfLink.applyIncoming(getFilesDir());
        if (applied > 0) {
            Log.i(TAG, "Put " + applied + " save file(s) from CubeShelf in place");
        }
        exportOnlineEnvironment(getIntent());
        super.onCreate(savedInstanceState);
        // Meta Quest: the game shows on a virtual screen in the headset instead
        // of this window, and the Touch controllers are its gamepad.
        if (mSurface != null && QuestVr.isHeadset()) {
            QuestVr.start(this, mSurface);
        }
        useDisplayCutout();
        hideSystemBars();
        // The game asks for its frame rate before the surface exists; a surface
        // without a rate vote is held to 60 FPS or less (48 on a 144 Hz screen).
        // Vote again each time Android creates or resizes the surface.
        if (mSurface != null) {
            mSurface.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
                @Override
                public void surfaceCreated(android.view.SurfaceHolder holder) {
                }

                @Override
                public void surfaceChanged(android.view.SurfaceHolder holder, int format, int width, int height) {
                    if (preferredFrameRate > 0f) {
                        setPreferredFrameRate(preferredFrameRate);
                    }
                }

                @Override
                public void surfaceDestroyed(android.view.SurfaceHolder holder) {
                }
            });
        }
    }

    // Called from the game's "Play Online" entry (src/port/ui/online.cpp) on
    // the SDL thread. The game then ends itself; the lobby restarts it for the
    // session, like the Windows companion does.
    public boolean openOnlineLobby(String nickname, String disc, String language, String invitation) {
        try {
            Intent lobby = new Intent(this, LobbyActivity.class);
            lobby.putExtra(LobbyActivity.EXTRA_NICKNAME, nickname);
            lobby.putExtra(LobbyActivity.EXTRA_DISC, disc);
            lobby.putExtra(LobbyActivity.EXTRA_LANGUAGE, language);
            if (invitation != null && !invitation.isEmpty()) {
                lobby.putExtra(LobbyActivity.EXTRA_INVITATION, invitation);
            }
            lobby.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(lobby);
            return true;
        } catch (RuntimeException e) {
            Log.e(TAG, "Unable to open the online lobby", e);
            return false;
        }
    }

    @Override
    protected void onDestroy() {
        boolean finishing = isFinishing();
        if (finishing && onlineSession) {
            // Back to the salon, which now says how the session ended.
            try {
                Intent lobby = new Intent(this, LobbyActivity.class);
                lobby.putExtra(LobbyActivity.EXTRA_LANGUAGE, onlineLanguage);
                lobby.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                startActivity(lobby);
            } catch (RuntimeException e) {
                Log.w(TAG, "Unable to return to the online lobby", e);
            }
        }
        super.onDestroy();
        // After SDL's onDestroy, which waits for the game: nothing draws into
        // the headset's surface any more.
        QuestVr.stop();
        if (finishing) {
            // SDL refuses to run main() twice in one process, and the next
            // start may carry different --netplay arguments: end this one.
            System.exit(0);
        }
    }

    // Draw beside the notch or camera hole as well. Android 15 does this by
    // default for apps targeting it; before that, a landscape game with hidden
    // system bars gets a black band on the cutout side. The screen controls
    // keep clear of the cutout through SDL's safe area.
    private void useDisplayCutout() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            return;
        }
        Window window = getWindow();
        WindowManager.LayoutParams attributes = window.getAttributes();
        attributes.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
            ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(attributes);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
            // A surface created again after the background forgets its frame rate.
            if (preferredFrameRate > 0f) {
                setPreferredFrameRate(preferredFrameRate);
            }
        }
    }

    private void hideSystemBars() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController ctrl = window.getDecorView().getWindowInsetsController();
            if (ctrl != null) {
                ctrl.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                ctrl.hide(WindowInsets.Type.systemBars());
            }
        } else {
            View decorView = window.getDecorView();
            int uiOptions = View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
            decorView.setSystemUiVisibility(uiOptions);
            ActionBar actionBar = getActionBar();
            if (actionBar != null) {
                actionBar.hide();
            }
        }
    }

    @Override
    public void onBackPressed() {
        // Back never closes Party Board: like F1 and B on a PC, it opens the
        // menu in game and goes back inside a menu (src/port/ui/input.cpp).
        // The menu has its own Quit entry.
        try {
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_BACK);
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_BACK);
        } catch (UnsatisfiedLinkError e) {
            super.onBackPressed();
        }
    }

    @Override
    protected String[] getLibraries() {
        // SDL3 is statically linked into libmain.so in this build.
        return new String[] {
            "main"
        };
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        if (intent != null) {
            String[] argv = intent.getStringArrayExtra("partyboard_argv");
            if (argv != null && argv.length > 0) {
                return argv;
            }

            String rawArgs = intent.getStringExtra("partyboard_args");
            if (rawArgs != null) {
                String trimmed = rawArgs.trim();
                if (!trimmed.isEmpty()) {
                    return splitArgs(trimmed);
                }
            }
        }
        return new String[0];
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (resultCode == RESULT_OK) {
            persistUriPermissions(data);
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    private void persistUriPermissions(Intent data) {
        if (data == null) {
            return;
        }

        int permissionFlags =
            data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if (permissionFlags == 0) {
            return;
        }

        Uri uri = data.getData();
        if (uri != null) {
            persistUriPermission(uri, permissionFlags);
        }

        ClipData clipData = data.getClipData();
        if (clipData == null) {
            return;
        }
        for (int i = 0; i < clipData.getItemCount(); ++i) {
            Uri itemUri = clipData.getItemAt(i).getUri();
            if (itemUri != null) {
                persistUriPermission(itemUri, permissionFlags);
            }
        }
    }

    private void persistUriPermission(Uri uri, int permissionFlags) {
        if ((permissionFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
            persistUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION, "read");
        }
        if ((permissionFlags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0) {
            persistUriPermission(uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION, "write");
        }
    }

    private void persistUriPermission(Uri uri, int permissionFlag, String permissionName) {
        try {
            getContentResolver().takePersistableUriPermission(uri, permissionFlag);
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to persist " + permissionName + " URI permission for " + uri, e);
        }
    }

    // Refresh rates the screen offers at its current resolution, for the Frame
    // Rate setting (src/port/display_rate.cpp). A 120 Hz phone answers 60 and
    // 120; offering 144 there would only be a number the screen cannot show.
    public float[] getSupportedRefreshRates() {
        if (QuestVr.isStarted()) {
            return QuestVr.refreshRates();
        }
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        List<Float> rates = new ArrayList<>();
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() != current.getPhysicalWidth()
                || mode.getPhysicalHeight() != current.getPhysicalHeight()) {
                continue;
            }
            float rate = Math.round(mode.getRefreshRate());
            if (!rates.contains(rate)) {
                rates.add(rate);
            }
        }
        float[] out = new float[rates.size()];
        for (int i = 0; i < out.length; i++) {
            out[i] = rates.get(i);
        }
        return out;
    }

    // Android caps a game at 60 FPS unless it asks for more, whatever the screen
    // can do, and a rate the screen cannot divide evenly (60 on a 144 Hz panel)
    // judders. Called from the SDL thread at start and when the Frame Rate
    // setting changes: pick the slowest mode that shows this rate exactly
    // (60 FPS on a 60 Hz mode rather than 120 Hz, for the battery), and tell
    // the surface the rate too, which variable-refresh (LTPO) screens use.
    private volatile float preferredFrameRate;

    private static boolean shows(float refresh, float fps) {
        float ratio = refresh / fps;
        return ratio >= 0.99f && Math.abs(ratio - Math.round(ratio)) < 0.02f;
    }

    public void setPreferredFrameRate(final float fps) {
        preferredFrameRate = fps;
        if (QuestVr.isStarted()) {
            // The headset's display, not this window's, shows the game.
            QuestVr.setFrameRate(fps);
            return;
        }
        runOnUiThread(() -> {
            Window window = getWindow();
            WindowManager.LayoutParams attributes = window.getAttributes();
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode current = display.getMode();
            Display.Mode exact = null;
            Display.Mode fastest = null;
            for (Display.Mode mode : display.getSupportedModes()) {
                if (mode.getPhysicalWidth() != current.getPhysicalWidth()
                    || mode.getPhysicalHeight() != current.getPhysicalHeight()) {
                    continue;
                }
                if (shows(mode.getRefreshRate(), fps) && (exact == null || mode.getRefreshRate() < exact.getRefreshRate())) {
                    exact = mode;
                }
                if (fastest == null || mode.getRefreshRate() > fastest.getRefreshRate()) {
                    fastest = mode;
                }
            }
            Display.Mode chosen = exact != null ? exact : (fps > 60.5f ? fastest : null);
            int modeId = chosen != null ? chosen.getModeId() : 0;
            if (attributes.preferredDisplayModeId != modeId) {
                attributes.preferredDisplayModeId = modeId;
                window.setAttributes(attributes);
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && mSurface != null) {
                Surface surface = mSurface.getHolder().getSurface();
                if (surface != null && surface.isValid()) {
                    try {
                        surface.setFrameRate(fps, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
                    } catch (IllegalArgumentException | IllegalStateException e) {
                        Log.w(TAG, "Surface frame rate not accepted", e);
                    }
                }
            }
            Log.i(TAG, "Preferred frame rate " + fps + " (display mode " + modeId
                + (chosen != null ? ", " + chosen.getRefreshRate() + " Hz" : "") + ")");
        });
    }

    // The build number the GitHub update manifest is compared with
    // (src/port/app_update.cpp, called from a worker thread).
    @SuppressWarnings("deprecation")
    public long getInstalledVersionCode() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.P ? info.getLongVersionCode() : info.versionCode;
        } catch (PackageManager.NameNotFoundException e) {
            return 0;
        }
    }

    public String getDisplayNameForUri(String uriString) {
        if (uriString == null || uriString.isEmpty()) {
            return "";
        }

        Uri uri = Uri.parse(uriString);
        if ("content".equals(uri.getScheme())) {
            try (Cursor cursor = getContentResolver().query(
                uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null))
            {
                if (cursor != null && cursor.moveToFirst()) {
                    int displayNameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (displayNameColumn >= 0) {
                        String displayName = cursor.getString(displayNameColumn);
                        if (displayName != null && !displayName.isEmpty()) {
                            return displayName;
                        }
                    }
                }
            } catch (SecurityException | IllegalArgumentException e) {
                Log.w(TAG, "Unable to query display name for " + uri, e);
            }
        } else if ("file".equals(uri.getScheme())) {
            String path = uri.getPath();
            if (path != null && !path.isEmpty()) {
                String name = new File(path).getName();
                if (!name.isEmpty()) {
                    return name;
                }
            }
        }

        String lastSegment = uri.getLastPathSegment();
        return lastSegment != null ? lastSegment : "";
    }
}
