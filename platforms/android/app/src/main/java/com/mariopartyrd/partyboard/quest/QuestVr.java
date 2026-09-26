package com.mariopartyrd.partyboard.quest;

import android.app.Activity;
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
 * Clicking the right thumbstick opens placement mode (the first start opens
 * it by itself): the game is put on the table, moved, sized, and its
 * resolution (up to 4K) and the room's visibility chosen. The headset keeps
 * the place (a spatial anchor) and the choices (quest_table.txt).
 */
public final class QuestVr {
    private static final String TAG = "PartyBoardQuest";

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
        surface.useExternalSurface();
        QuestControllers.install(QuestVr::rumble);
        QuestUpdater.install(activity);
        String state = new File(activity.getFilesDir(), "quest_table.txt").getAbsolutePath();
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

    private static void onPlacement(boolean placing, int resolution, boolean passthrough, boolean model) {
        final Activity activity = sActivity;
        if (placing && activity != null) {
            activity.runOnUiThread(() -> drawHelp(resolution, passthrough, model));
        }
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
        switch (height) {
            case 2160:
                return "4K (3840×2160)";
            case 1440:
                return "1440p (2560×1440)";
            default:
                return "1080p (1920×1080)";
        }
    }

    // The panel under the line of sight while placing: the controls, and the
    // current resolution, room visibility and model.
    private static void drawHelp(int resolution, boolean passthrough, boolean model) {
        Surface surface = sHelpSurface;
        if (surface == null || !surface.isValid()) {
            return;
        }
        boolean french = "fr".equals(Locale.getDefault().getLanguage());
        String title = french ? "Placer le jeu" : "Place the game";
        String[] lines = french ? new String[] {
            "Gâchette droite : poser le jeu au bout de la manette",
            "Grip droit (maintenu) : déplacer",
            "Stick droit : ↕ taille de l'écran   ↔ tourner",
            "Stick gauche : ↕ hauteur   ↔ taille du plateau",
            "Clic stick gauche : plateau 3D — " + (model ? "oui" : "non"),
            "X : pièce visible — " + (passthrough ? "oui" : "non"),
            "Y : résolution — " + resolutionName(resolution),
            "A, B ou \u2261 : terminer et jouer",
        } : new String[] {
            "Right trigger: set the game at the controller's tip",
            "Right grip (hold): move",
            "Right stick: ↕ screen size   ↔ turn",
            "Left stick: ↕ height   ↔ board size",
            "Left stick click: 3D board — " + (model ? "yes" : "no"),
            "X: room visible — " + (passthrough ? "yes" : "no"),
            "Y: resolution — " + resolutionName(resolution),
            "A, B or \u2261: done, play",
        };

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
            text.setTextSize(30 * scale);
            float y = 175 * scale;
            for (String line : lines) {
                canvas.drawText(line, 48 * scale, y, text);
                y += 53 * scale;
            }
        } finally {
            surface.unlockCanvasAndPost(canvas);
        }
    }

    private static native boolean nativeStart(Activity activity, String statePath);
    private static native void nativeStop();
    private static native void nativeSurfaceSwitched();
    private static native void nativeRumble(float amplitude, int durationMs);
    private static native float[] nativeRefreshRates();
    private static native void nativeRequestRefreshRate(float rate);
}
