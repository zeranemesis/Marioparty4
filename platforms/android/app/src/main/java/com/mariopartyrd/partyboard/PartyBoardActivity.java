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
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import com.mariopartyrd.partyboard.online.LobbyActivity;
import com.mariopartyrd.partyboard.online.OnlineService;

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
        } catch (ErrnoException e) {
            Log.w(TAG, "Unable to pass the online session to the game", e);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        exportOnlineEnvironment(getIntent());
        super.onCreate(savedInstanceState);
        useDisplayCutout();
        hideSystemBars();
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
