package org.libsdl.app;

import android.view.KeyEvent;

/**
 * Meta Quest: the two Touch controllers, read through OpenXR (QuestVr), shown
 * to SDL as one Android gamepad. Aurora then maps it to the GameCube pad like
 * any other controller, and the Party Board menus navigate with it.
 *
 *   right A / B, left X / Y  -> A, B, X, Y
 *   left thumbstick          -> control stick (D-pad while the left grip is held)
 *   right thumbstick         -> C stick
 *   left / right trigger     -> L / R (analog)
 *   right grip               -> Z
 *   left menu button         -> Start
 *   left thumbstick click    -> Select: the Party Board menu (F1 on a PC)
 *   right thumbstick click   -> not the game's: placement mode (QuestVr)
 *
 * In this package because SDL's joystick entry points are package-private.
 */
public final class QuestControllers {
    // Same bits as quest_xr.cpp.
    public static final int BUTTON_A = 1 << 0;
    public static final int BUTTON_B = 1 << 1;
    public static final int BUTTON_X = 1 << 2;
    public static final int BUTTON_Y = 1 << 3;
    public static final int BUTTON_MENU = 1 << 4;
    public static final int BUTTON_LEFT_STICK = 1 << 5;
    public static final int BUTTON_RIGHT_STICK = 1 << 6;

    /** Receives the game's rumble (the phone vibrator on other devices). */
    public interface Rumble {
        void rumble(float intensity, int lengthMs);
    }

    // Outside Android's input device ids, which SDL uses for real gamepads.
    private static final int DEVICE_ID = 0x5155E57;
    // Aurora rumbles the device's own vibrator (lib/device.cpp); SDL knows it by this id.
    private static final int VIBRATOR_SERVICE_ID = 999999;
    private static final int OCULUS_VENDOR_ID = 0x2833;
    private static final int TOUCH_PRODUCT_ID = 0x0001;

    // SDL_GAMEPAD_BUTTON_* bits for SDLJoystickHandler.getButtonMask()'s mapping:
    // A B X Y, Back (Select), Start, both stick clicks, both shoulders, D-pad.
    private static final int BUTTON_MASK = 0x7FDF;
    // Left stick, right stick, both triggers.
    private static final int AXIS_MASK = 0x003F;
    private static final int AXIS_COUNT = 6;

    private static final float PRESS = 0.5f;
    private static final float RELEASE = 0.35f;

    private static final int[] KEYCODES = {
        KeyEvent.KEYCODE_BUTTON_A,
        KeyEvent.KEYCODE_BUTTON_B,
        KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_Y,
        KeyEvent.KEYCODE_BUTTON_START,
        KeyEvent.KEYCODE_BUTTON_SELECT,
        KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT,
        KeyEvent.KEYCODE_DPAD_RIGHT,
    };

    private static final boolean[] sPressed = new boolean[KEYCODES.length];
    private static volatile boolean sAdded;
    private static Rumble sRumble;

    private QuestControllers() {
    }

    private static volatile boolean sInstalled;

    /**
     * The headset's own Android input devices for the Touch controllers
     * (Oculus vendor id): once OpenXR reads them, they would only add players
     * that never move. Bluetooth gamepads are kept.
     */
    static boolean ignores(android.view.InputDevice device) {
        return sInstalled && device.getVendorId() == OCULUS_VENDOR_ID;
    }

    /** Called once the activity exists (SDLControllerManager is initialized). */
    public static void install(Rumble rumble) {
        sInstalled = true;
        sRumble = rumble;
        SDLControllerManager.mJoystickHandler = new JoystickHandler();
        SDLControllerManager.mHapticHandler = new HapticHandler(SDLControllerManager.mHapticHandler);
    }

    /**
     * One controller reading, from the OpenXR thread, every headset frame.
     * Sticks are -1..1 with up positive (OpenXR), triggers and grips 0..1.
     */
    public static synchronized void update(int buttons, float leftX, float leftY, float rightX, float rightY,
                                           float leftTrigger, float rightTrigger, float leftGrip, float rightGrip) {
        if (!sAdded) {
            return;
        }
        // The left grip turns the stick into a D-pad (board menus, a few
        // minigames); the control stick then rests.
        final boolean dpad = leftGrip >= PRESS;
        final boolean[] down = {
            (buttons & BUTTON_A) != 0,
            (buttons & BUTTON_B) != 0,
            (buttons & BUTTON_X) != 0,
            (buttons & BUTTON_Y) != 0,
            (buttons & BUTTON_MENU) != 0,
            (buttons & BUTTON_LEFT_STICK) != 0,
            analog(6, rightGrip),
            dpad && analog(7, leftY),
            dpad && analog(8, -leftY),
            dpad && analog(9, -leftX),
            dpad && analog(10, leftX),
        };
        for (int i = 0; i < KEYCODES.length; ++i) {
            if (down[i] != sPressed[i]) {
                sPressed[i] = down[i];
                if (down[i]) {
                    SDLControllerManager.onNativePadDown(DEVICE_ID, KEYCODES[i], 0);
                } else {
                    SDLControllerManager.onNativePadUp(DEVICE_ID, KEYCODES[i], 0);
                }
            }
        }

        // Android's axis order (SDL_CreateMappingForAndroidGamepad): left X/Y,
        // right X/Y, left/right trigger. Down is positive, and a released
        // trigger is -1. Sent every frame: SDL drops repeats, and the first
        // reading after SDL opens the pad replaces its centered triggers.
        SDLControllerManager.onNativeJoy(DEVICE_ID, 0, dpad ? 0.0f : leftX);
        SDLControllerManager.onNativeJoy(DEVICE_ID, 1, dpad ? 0.0f : -leftY);
        SDLControllerManager.onNativeJoy(DEVICE_ID, 2, rightX);
        SDLControllerManager.onNativeJoy(DEVICE_ID, 3, -rightY);
        SDLControllerManager.onNativeJoy(DEVICE_ID, 4, leftTrigger * 2.0f - 1.0f);
        SDLControllerManager.onNativeJoy(DEVICE_ID, 5, rightTrigger * 2.0f - 1.0f);
    }

    /** Everything released: the headset menu took the controllers, or it was taken off. */
    public static void release() {
        update(0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    // A digital input from an analog one, with some hysteresis so a grip or a
    // stick held near the threshold does not chatter.
    private static boolean analog(int index, float value) {
        return sPressed[index] ? value >= RELEASE : value >= PRESS;
    }

    // SDL asks for its gamepads when its joystick subsystem starts, then every
    // few seconds: add the Touch controllers the first time. Real gamepads
    // (Bluetooth) keep working beside them.
    static final class JoystickHandler extends SDLJoystickHandler {
        @Override
        synchronized void pollInputDevices() {
            super.pollInputDevices();
            if (!sAdded) {
                SDLControllerManager.nativeAddJoystick(DEVICE_ID, "Meta Quest Touch", "meta-quest-touch",
                    OCULUS_VENDOR_ID, TOUCH_PRODUCT_ID, BUTTON_MASK, AXIS_COUNT, AXIS_MASK, 0,
                    false, false, false, false);
                sAdded = true;
            }
        }
    }

    // The headset has no vibrator of its own: the game's rumble goes to both
    // controllers. Gamepad rumble stays with SDL's own handler.
    static final class HapticHandler extends SDLHapticHandler {
        private final SDLHapticHandler mFallback;
        private boolean mAdded;

        HapticHandler(SDLHapticHandler fallback) {
            mFallback = fallback;
        }

        @Override
        synchronized void pollHapticDevices() {
            if (!mAdded) {
                SDLControllerManager.nativeAddHaptic(VIBRATOR_SERVICE_ID, "VIBRATOR_SERVICE");
                mAdded = true;
            }
        }

        @Override
        void run(int device_id, float intensity, int length) {
            if (device_id == VIBRATOR_SERVICE_ID) {
                if (sRumble != null) {
                    sRumble.rumble(intensity, length);
                }
            } else if (mFallback != null) {
                mFallback.run(device_id, intensity, length);
            }
        }

        @Override
        void rumble(int device_id, float low_frequency_intensity, float high_frequency_intensity, int length) {
            if (mFallback != null) {
                mFallback.rumble(device_id, low_frequency_intensity, high_frequency_intensity, length);
            }
        }

        @Override
        void stop(int device_id) {
            if (device_id == VIBRATOR_SERVICE_ID) {
                if (sRumble != null) {
                    sRumble.rumble(0.0f, 0);
                }
            } else if (mFallback != null) {
                mFallback.stop(device_id);
            }
        }
    }
}
