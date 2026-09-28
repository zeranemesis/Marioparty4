package com.mariopartyrd.partyboard.quest;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Build;
import android.util.Log;
import android.view.Surface;

import org.libsdl.app.QuestControllers;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.util.Locale;

/**
 * Meta Quest 3 (and 2): Party Board in mixed reality, standing on the player's
 * table, played with the Touch controllers (libpartyboard_quest.so,
 * quest_xr.cpp).
 *
 * The manifest's VR category makes Horizon OS start the activity immersive;
 * phones ignore it and never get here. The game renders exactly as on a
 * phone, into the OpenXR surface given to SDL in place of the SurfaceView's.
 *
 * Before the first board or minigame, the game waits for a one-time table
 * calibration: the right controller laid at the table's center. The table of
 * the headset's room scan under it (the spatial data permission, asked then)
 * gives the height and the model's size. Clicking the right thumbstick opens
 * manual placement mode: the game can be moved, sized, and its resolution (up
 * to 4K), the room's visibility and the model following the game camera
 * chosen. The headset keeps the place (a spatial anchor) and the choices
 * (quest_table.txt).
 */
public final class QuestVr {
    private static final String TAG = "PartyBoardQuest";

    // The room scan (Space Setup's tables), for the table calibration.
    private static final String SCENE_PERMISSION = "com.oculus.permission.USE_SCENE";
    private static final int SCENE_PERMISSION_REQUEST = 0x5C3E;

    // quest_xr.cpp's TableScene::State.
    private static final int SCENE_UNAVAILABLE = 0;
    private static final int SCENE_NO_PERMISSION = 1;
    private static final int SCENE_SEARCHING = 2;
    private static final int SCENE_NO_TABLE = 3;
    private static final int SCENE_FOUND = 4;
    private static final int SCENE_CAPTURING = 5;

    // Menu scale: a 1080p desktop at 125%, readable in the headset; a higher
    // resolution scales the menus with it so they keep their size.
    private static final float DENSITY_AT_1080P = 1.25f;

    private static Activity sActivity;
    private static SDLSurface sSurface;
    private static volatile boolean sStarted;
    private static volatile float sRefreshRate = 72.0f;

    private static Surface sHelpSurface;
    private static int sHelpWidth;
    private static int sHelpHeight;

    private QuestVr() {
    }

    /** A Meta Quest headset (Horizon OS), where the game runs in the headset. */
    public static boolean isHeadset() {
        String manufacturer = Build.MANUFACTURER;
        return "Oculus".equalsIgnoreCase(manufacturer) || "Meta".equalsIgnoreCase(manufacturer);
    }

    /**
     * Starts the headset session. The game starts once its surface exists
     * (onSurface), as it does on a phone once the SurfaceView's exists.
     */
    public static boolean start(Activity activity, SDLSurface surface) {
        if (sStarted) {
            return true;
        }
        try {
            System.loadLibrary("partyboard_quest");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Quest support missing from this build", e);
            return false;
        }
        sActivity = activity;
        sSurface = surface;
        QuestLog.start(activity);
        surface.useExternalSurface();
        QuestControllers.install(QuestVr::rumble);
        QuestUpdater.install(activity);
        String state = new File(activity.getFilesDir(), "quest_table.txt").getAbsolutePath();
        nativeScenePermission(activity.checkSelfPermission(SCENE_PERMISSION) == PackageManager.PERMISSION_GRANTED);
        sStarted = nativeStart(activity, state);
        if (!sStarted) {
            Log.e(TAG, "Unable to start the headset session");
        }
        return sStarted;
    }

    public static boolean isStarted() {
        return sStarted;
    }

    public static void stop() {
        if (!sStarted) {
            return;
        }
        sStarted = false;
        nativeStop();
        sActivity = null;
        sSurface = null;
        sHelpSurface = null;
    }

    /** Headset refresh rates (72, 80, 90, 120 Hz...), for the game's Frame Rate setting. */
    public static float[] refreshRates() {
        return sStarted ? nativeRefreshRates() : new float[0];
    }

    /** Request the fastest mode advertised by the active XR runtime. */
    public static void setFrameRate(float fps) {
        float chosen = 0.0f;
        for (float rate : refreshRates()) {
            chosen = Math.max(chosen, rate);
        }
        if (chosen > 0.0f) {
            Log.i(TAG, "Frame rate " + fps + " on a " + chosen + " Hz headset display");
            sRefreshRate = chosen;
            nativeRequestRefreshRate(chosen);
        }
    }

    private static float density(int height) {
        return DENSITY_AT_1080P * height / 1080.0f;
    }

    private static void rumble(float intensity, int lengthMs) {
        if (sStarted) {
            nativeRumble(intensity, lengthMs);
        }
    }

    /**
     * PartyBoardActivity's permission results: true when it was the spatial
     * data permission asked for the table calibration.
     */
    public static boolean onRequestPermissionsResult(int requestCode, int[] grantResults) {
        if (requestCode != SCENE_PERMISSION_REQUEST) {
            return false;
        }
        boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        Log.i(TAG, "Spatial data permission " + (granted ? "granted" : "refused"));
        if (sStarted) {
            nativeScenePermission(granted);
        }
        return true;
    }

    // --- From the OpenXR thread (quest_xr.cpp) ---

    private static void onSurface(Surface surface, int width, int height, float refreshRate) {
        final Activity activity = sActivity;
        final SDLSurface sdlSurface = sSurface;
        if (activity == null || sdlSurface == null) {
            return;
        }
        if (surface == null) {
            // No OpenXR runtime, or it refused the session: nothing can show
            // in the headset, so leave rather than stay in an empty void.
            Log.e(TAG, "No virtual screen: the game cannot be shown in the headset");
            activity.runOnUiThread(activity::finish);
            return;
        }
        sRefreshRate = refreshRate;
        activity.runOnUiThread(() ->
            sdlSurface.setExternalSurface(surface, width, height, density(height), refreshRate));
    }

    // The player chose another resolution: SDL moves to the new surface, then
    // the headset shows it and drops the old one.
    private static void onSurfaceReplaced(Surface surface, int width, int height) {
        final Activity activity = sActivity;
        final SDLSurface sdlSurface = sSurface;
        if (activity == null || sdlSurface == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            sdlSurface.replaceExternalSurface(surface, width, height, density(height), sRefreshRate);
            nativeSurfaceSwitched();
        });
    }

    private static void onHelpSurface(Surface surface, int width, int height) {
        sHelpSurface = surface;
        sHelpWidth = width;
        sHelpHeight = height;
    }

    private static void onPlacement(boolean placing, int resolution, boolean passthrough, boolean model,
                                    boolean calibrating, boolean calibrationConfirmed, boolean followCamera,
                                    int sceneState, float tableLength, float tableWidth) {
        final Activity activity = sActivity;
        if (placing && activity != null) {
            activity.runOnUiThread(() -> drawHelp(resolution, passthrough, model, calibrating,
                                                  calibrationConfirmed, followCamera, sceneState,
                                                  tableLength, tableWidth));
        }
    }

    // The table calibration opened: Horizon OS shows its own permission panel
    // for the room scan, once.
    private static void requestScenePermission() {
        final Activity activity = sActivity;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            if (activity.checkSelfPermission(SCENE_PERMISSION) == PackageManager.PERMISSION_GRANTED) {
                nativeScenePermission(true);
            } else {
                activity.requestPermissions(new String[] {SCENE_PERMISSION}, SCENE_PERMISSION_REQUEST);
            }
        });
    }

    private static void onSessionState(boolean running, boolean focused) {
        if (!focused) {
            QuestControllers.release();
        }
    }

    private static void onControllers(int buttons, float leftX, float leftY, float rightX, float rightY,
                                      float leftTrigger, float rightTrigger, float leftGrip, float rightGrip) {
        QuestControllers.update(buttons, leftX, leftY, rightX, rightY, leftTrigger, rightTrigger, leftGrip, rightGrip);
    }

    // Quit from the headset's menu.
    private static void onExit() {
        final Activity activity = sActivity;
        if (activity != null) {
            activity.runOnUiThread(activity::finish);
        }
    }

    // --- Placement help ---

    private static String resolutionName(int height) {
        boolean french = "fr".equals(Locale.getDefault().getLanguage());
        switch (height) {
            case 2160:
                return french ? "Nettet\u00e9 : 125% 3D, \u00e9cran 4K" : "Sharp: 125% 3D, 4K screen";
            case 1440:
                return french ? "\u00c9quilibr\u00e9 : 100% 3D, \u00e9cran 1440p" : "Balanced: 100% 3D, 1440p screen";
            default:
                return french ? "\u00c9conomie : 80% 3D, \u00e9cran 1080p" : "Economy: 80% 3D, 1080p screen";
        }
    }

    // What the room scan found, for the calibration panel.
    private static String sceneLine(boolean french, int sceneState, float length, float width) {
        switch (sceneState) {
            case SCENE_FOUND:
                return String.format(Locale.getDefault(),
                    french ? "Table scannée : %.2f × %.2f m, le plateau s'y adaptera."
                           : "Scanned table: %.2f × %.2f m, the board will fit it.", length, width);
            case SCENE_NO_TABLE:
                return french ? "Aucune table dans le scan de la pièce."
                              : "No table in the room scan.";
            case SCENE_NO_PERMISSION:
                return french ? "Autorisez « Données spatiales » pour utiliser le scan de la pièce."
                              : "Allow \"Spatial data\" to use the room scan.";
            case SCENE_SEARCHING:
                return french ? "Recherche de la table scannée…" : "Looking for the scanned table…";
            case SCENE_CAPTURING:
                return french ? "Scan de la pièce en cours…" : "Scanning the room…";
            case SCENE_UNAVAILABLE:
            default:
                return french ? "Scan de la pièce indisponible : taille du plateau au stick gauche."
                              : "Room scan unavailable: board size on the left stick.";
        }
    }

    // The panel under the line of sight while placing: the controls, and the
    // current resolution, room visibility and model. While calibrating: the
    // controller to lay on the table, and what the room scan found.
    private static void drawHelp(int resolution, boolean passthrough, boolean model, boolean calibrating,
                                 boolean calibrationConfirmed, boolean followCamera, int sceneState,
                                 float tableLength, float tableWidth) {
        Surface surface = sHelpSurface;
        if (surface == null || !surface.isValid()) {
            return;
        }
        boolean french = "fr".equals(Locale.getDefault().getLanguage());
        String title = calibrating ? (french ? "Avant de jouer : votre table" : "Before playing: your table")
                                  : (french ? "Placer le jeu" : "Place the game");
        boolean canScan = sceneState != SCENE_UNAVAILABLE && sceneState != SCENE_NO_PERMISSION
            && sceneState != SCENE_CAPTURING;
        String keys = canScan ? (french ? "A : scanner la pièce   ·   B : plus tard"
                                        : "A: scan the room   ·   B: later")
                              : (french ? "B : plus tard" : "B: later");
        String scene = sceneLine(french, sceneState, tableLength, tableWidth);
        String[] lines = calibrating ? (french ? (calibrationConfirmed ? new String[] {
            "Mesure en cours : gardez la manette droite immobile.",
            "La pose se verrouille après une mesure stable.",
            scene,
            keys,
        } : new String[] {
            "Posez la manette droite à plat, au milieu de la table.",
            "Orientez l'avant vers vous, puis appuyez sur X.",
            "Gardez-la immobile pendant la mesure.",
            scene,
            keys,
        }) : (calibrationConfirmed ? new String[] {
            "Measuring: keep the right controller still.",
            "The placement locks after a stable reading.",
            scene,
            keys,
        } : new String[] {
            "Lay the right controller flat at the middle of the table.",
            "Point its front toward you, then press X.",
            "Keep it still during the measurement.",
            scene,
            keys,
        })) : (french ? new String[] {
            "Gâchettes droite/gauche : poser le jeu / hauteur de table",
            "Grip droit (maintenu) : déplacer",
            "Stick droit : ↕ taille de l'écran   ↔ tourner",
            "Stick gauche : ↕ hauteur   ↔ taille du plateau",
            "Clic stick gauche : plateau 3D — " + (model ? "oui" : "non"),
            "Grip gauche : mini-jeux sur l'écran, vus comme à la télé — " + (followCamera ? "oui" : "non"),
            "X : pièce visible — " + (passthrough ? "oui" : "non"),
            "Y : qualité — " + resolutionName(resolution),
            "A : recalibrer avec la manette droite",
            "B, \u2261 ou clic stick droit : terminer",
        } : new String[] {
            "Right/left trigger: place game / adjust table height",
            "Right grip (hold): move",
            "Right stick: ↕ screen size   ↔ turn",
            "Left stick: ↕ height   ↔ board size",
            "Left stick click: 3D board — " + (model ? "yes" : "no"),
            "Left grip: minigames on the screen, seen as on TV — " + (followCamera ? "yes" : "no"),
            "X: room visible — " + (passthrough ? "yes" : "no"),
            "Y: quality — " + resolutionName(resolution),
            "A: recalibrate with the right controller",
            "B, \u2261 or right stick click: done, play",
        });

        Canvas canvas;
        try {
            canvas = surface.lockHardwareCanvas();
        } catch (IllegalArgumentException | IllegalStateException e) {
            Log.w(TAG, "Placement help unavailable", e);
            return;
        }
        try {
            float scale = sHelpWidth / 1024.0f;
            canvas.drawColor(Color.rgb(24, 26, 36));
            Paint accent = new Paint(Paint.ANTI_ALIAS_FLAG);
            accent.setColor(Color.rgb(90, 160, 255));
            canvas.drawRect(0, 0, sHelpWidth, 10 * scale, accent);

            Paint heading = new Paint(Paint.ANTI_ALIAS_FLAG);
            heading.setColor(Color.WHITE);
            heading.setTextSize(56 * scale);
            heading.setTypeface(Typeface.DEFAULT_BOLD);
            canvas.drawText(title, 48 * scale, 100 * scale, heading);

            Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
            text.setColor(Color.rgb(225, 228, 240));
            text.setTextSize(28 * scale);
            float y = 160 * scale;
            for (String line : lines) {
                canvas.drawText(line, 48 * scale, y, text);
                y += 44 * scale;
            }

            // How the headset keeps up (the last 5 seconds), and where the log is.
            Paint small = new Paint(Paint.ANTI_ALIAS_FLAG);
            small.setColor(Color.rgb(150, 160, 185));
            small.setTextSize(22 * scale);
            y += 6 * scale;
            canvas.drawText(perfLine(french), 48 * scale, y, small);
            canvas.drawText(perfDetails(french), 48 * scale, y + 28 * scale, small);
            canvas.drawText(french ? "Journal : Téléchargements › PartyBoard (appli Fichiers)"
                                   : "Log: Downloads › PartyBoard (Files app)", 48 * scale, y + 56 * scale, small);
        } finally {
            surface.unlockCanvasAndPost(canvas);
        }
    }

    // perf_metrics.cpp's numbers: refresh rate, resolution %, GPU %, CPU %,
    // late frames %, app GPU ms, app CPU ms; negative when unknown.
    private static String perfLine(boolean french) {
        float[] n = sStarted ? nativePerfNumbers() : new float[0];
        if (n.length < 7 || n[0] <= 0) {
            return french ? "Performances : mesure en cours…" : "Performance: measuring…";
        }
        StringBuilder line = new StringBuilder(french ? "Performances : " : "Performance: ");
        line.append(String.format(Locale.getDefault(), "%.0f Hz · ", n[0]));
        line.append(String.format(Locale.getDefault(), french ? "résolution %.0f %%" : "resolution %.0f%%", n[1]));
        if (n.length >= 10) {
            line.append(String.format(Locale.getDefault(), " \u00b7 %.0f\u00d7%.0f \u00b7 3D %.0f FPS", n[7], n[8], n[9]));
        }
        return line.toString();
    }

    private static String perfDetails(boolean french) {
        float[] n = sStarted ? nativePerfNumbers() : new float[0];
        if (n.length < 7 || n[0] <= 0) return "";
        StringBuilder line = new StringBuilder();
        if (n[2] >= 0) line.append(String.format(Locale.getDefault(), french ? " · GPU %.0f %%" : " · GPU %.0f%%", n[2]));
        if (n[3] >= 0) line.append(String.format(Locale.getDefault(), french ? " · CPU %.0f %%" : " · CPU %.0f%%", n[3]));
        line.append(String.format(Locale.getDefault(), french ? " · images en retard %.1f %%" : " · late frames %.1f%%", n[4]));
        return line.toString();
    }

    private static native boolean nativeStart(Activity activity, String statePath);
    private static native void nativeStop();
    private static native void nativeSurfaceSwitched();
    private static native void nativeScenePermission(boolean granted);
    private static native void nativeRumble(float amplitude, int durationMs);
    private static native float[] nativeRefreshRates();
    private static native void nativeRequestRefreshRate(float rate);
    private static native float[] nativePerfNumbers();
}
