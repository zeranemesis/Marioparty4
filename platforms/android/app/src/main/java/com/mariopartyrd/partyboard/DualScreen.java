package com.mariopartyrd.partyboard;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Path;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.Locale;

/**
 * Second screen of dual-screen handhelds (AYN Thor and the like): a live map of the
 * board while a party is on, and the settings worth changing mid-game, by touch.
 * The game keeps the controller: the window never takes key focus.
 * Native side: src/port/dual_screen.cpp.
 */
final class DualScreen {
    private static final String TAG = "PartyBoardDualScreen";

    private static native int nativeBoardSnapshot(float[] out);
    private static native int nativeGetSetting(String key);
    private static native void nativeSetSetting(String key, int value);
    private static native int[] nativeFrameRates();
    private static native int nativeImageGeneration();
    private static native int[] nativeImage(int id, int[] size);

    // Layout of the snapshot, as written by src/port/dual_screen.cpp.
    private static final int HEADER = 7;
    private static final int PLAYER_STRIDE = 6;
    private static final int SPACE_STRIDE = 7;
    private static final int MAX_FLOATS = HEADER + 4 * PLAYER_STRIDE + 256 * SPACE_STRIDE;

    private static Screen screen;

    private DualScreen() {}

    static void show(Activity activity) {
        if (screen != null) {
            return;
        }
        Display display = secondaryDisplay(activity);
        if (display == null) {
            return;
        }
        try {
            nativeBoardSnapshot(new float[1]);
        } catch (UnsatisfiedLinkError e) {
            Log.i(TAG, "Engine without a second-screen companion", e);
            return;
        }
        try {
            screen = new Screen(activity, display);
            screen.show();
        } catch (RuntimeException e) {
            Log.w(TAG, "Unable to show the second screen", e);
            screen = null;
        }
    }

    static void hide() {
        if (screen != null) {
            screen.dismiss();
            screen = null;
        }
    }

    private static Display secondaryDisplay(Context context) {
        DisplayManager manager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        if (manager == null) {
            return null;
        }
        for (Display display : manager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (display.getDisplayId() != Display.DEFAULT_DISPLAY) {
                return display;
            }
        }
        return null;
    }

    private static boolean french() {
        return "fr".equals(Locale.getDefault().getLanguage());
    }

    private static String tr(String english, String french) {
        return french() ? french : english;
    }

    private static final class Screen extends Presentation {
        private final Handler handler = new Handler(Looper.getMainLooper());
        private final float[] snapshot = new float[MAX_FLOATS];
        private BoardMapView map;
        private LinearLayout settings;
        private Button mapTab;
        private Button settingsTab;
        private boolean wasOnBoard;
        // The engine registers its settings only once the game starts: until then, retry.
        private boolean settingsReady;
        private int imageGeneration;
        private boolean imagesSupported = true;

        private final Runnable poll = new Runnable() {
            @Override
            public void run() {
                int size = nativeBoardSnapshot(snapshot);
                boolean onBoard = size > 1 && snapshot[1] != 0;
                if (onBoard && !wasOnBoard) {
                    select(true);
                }
                wasOnBoard = onBoard;
                if (map.getVisibility() == View.VISIBLE) {
                    refreshImages();
                    map.update(snapshot, size);
                } else if (!settingsReady) {
                    buildSettings();
                }
                handler.postDelayed(this, map.getVisibility() == View.VISIBLE ? 33 : 500);
            }
        };

        Screen(Context outer, Display display) {
            super(outer, display);
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            // Touch only: key focus stays with the game on the top screen.
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

            LinearLayout root = new LinearLayout(getContext());
            root.setOrientation(LinearLayout.VERTICAL);
            root.setBackgroundColor(0xFF10142A);

            LinearLayout tabs = new LinearLayout(getContext());
            tabs.setOrientation(LinearLayout.HORIZONTAL);
            tabs.setPadding(dp(12), dp(12), dp(12), dp(4));
            mapTab = tabButton(tr("Board map", "Carte du plateau"));
            settingsTab = tabButton(tr("Settings", "Réglages"));
            mapTab.setOnClickListener(v -> select(true));
            settingsTab.setOnClickListener(v -> select(false));
            tabs.addView(mapTab, new LinearLayout.LayoutParams(0, dp(56), 1));
            tabs.addView(settingsTab, new LinearLayout.LayoutParams(0, dp(56), 1));
            root.addView(tabs);

            FrameLayout content = new FrameLayout(getContext());
            map = new BoardMapView(getContext());
            content.addView(map);
            ScrollView scroll = new ScrollView(getContext());
            settings = new LinearLayout(getContext());
            settings.setOrientation(LinearLayout.VERTICAL);
            settings.setPadding(dp(16), dp(8), dp(16), dp(16));
            scroll.addView(settings);
            content.addView(scroll);
            root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
            setContentView(root);
            select(false);
        }

        @Override
        protected void onStart() {
            super.onStart();
            handler.post(poll);
        }

        @Override
        protected void onStop() {
            handler.removeCallbacks(poll);
            super.onStop();
        }

        // The game's own space icons and portraits, once the board has loaded them.
        private void refreshImages() {
            if (!imagesSupported) {
                return;
            }
            try {
                int generation = nativeImageGeneration();
                if (generation == imageGeneration) {
                    return;
                }
                imageGeneration = generation;
                Bitmap[] images = new Bitmap[11];
                int[] dims = new int[2];
                for (int id = 0; id < images.length; ++id) {
                    int[] pixels = nativeImage(id, dims);
                    if (pixels != null && dims[0] > 0 && dims[1] > 0) {
                        images[id] = Bitmap.createBitmap(pixels, dims[0], dims[1], Bitmap.Config.ARGB_8888);
                    }
                }
                map.setImages(images);
            } catch (UnsatisfiedLinkError e) {
                imagesSupported = false; // an engine from before the game images: keep the drawn map
            }
        }

        private void select(boolean showMap) {
            map.setVisibility(showMap ? View.VISIBLE : View.GONE);
            ((View) settings.getParent()).setVisibility(showMap ? View.GONE : View.VISIBLE);
            styleTab(mapTab, showMap);
            styleTab(settingsTab, !showMap);
            if (!showMap) {
                buildSettings();
            }
        }

        private void buildSettings() {
            settings.removeAllViews();
            settingsReady = nativeGetSetting("video.targetFrameRate") >= 0;
            section(tr("Display", "Affichage"));
            toggle(tr("Adaptive widescreen", "Écran large adaptatif"),
                tr("Fills the screen without stretching the scene.", "Remplit l'écran sans étirer la scène."),
                "video.enableAdaptiveWidescreen");
            toggle(tr("Original 4:3 picture", "Image 4:3 d'origine"),
                tr("Keeps the GameCube's proportions, with bars.", "Garde les proportions de la GameCube, avec des bandes."),
                "video.lockAspectRatio");
            int[] rates;
            try {
                rates = nativeFrameRates();
            } catch (UnsatisfiedLinkError e) {
                rates = new int[] {60};
            }
            String[] rateNames = new String[rates.length];
            for (int i = 0; i < rates.length; ++i) {
                rateNames[i] = rates[i] + tr(" FPS", " images/s");
            }
            choice(tr("Frame rate", "Images par seconde"), "video.targetFrameRate", rates, rateNames);
            toggle(tr("Frame counter", "Compteur d'images"), null, "game.enableFpsOverlay");
            section(tr("Controls", "Commandes"));
            choice(tr("On-screen controller", "Manette à l'écran"), "game.touchControls", new int[] {0, 1, 2},
                new String[] {tr("Automatic", "Automatique"), tr("Always", "Toujours"), tr("Never", "Jamais")});
        }

        private void section(String title) {
            TextView text = new TextView(getContext());
            text.setText(title.toUpperCase(Locale.getDefault()));
            text.setTextColor(0xFF8FA2FF);
            text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            text.setTypeface(Typeface.DEFAULT_BOLD);
            text.setPadding(dp(4), dp(18), 0, dp(6));
            settings.addView(text);
        }

        private void toggle(String label, String help, String key) {
            int value = nativeGetSetting(key);
            if (value < 0) {
                return;
            }
            row(label, help, value != 0 ? tr("On", "Activé") : tr("Off", "Désactivé"), value != 0, () -> {
                nativeSetSetting(key, nativeGetSetting(key) != 0 ? 0 : 1);
                // Widescreen and 4:3 switch each other off: redraw every row.
                handler.postDelayed(this::buildSettings, 50);
            });
        }

        private void choice(String label, String key, int[] values, String[] names) {
            int value = nativeGetSetting(key);
            if (value < 0) {
                return;
            }
            int current = 0;
            for (int i = 0; i < values.length; ++i) {
                if (values[i] == value) {
                    current = i;
                }
            }
            final int next = (current + 1) % values.length;
            row(label, null, names[current], false, () -> {
                nativeSetSetting(key, values[next]);
                handler.postDelayed(this::buildSettings, 50);
            });
        }

        private void row(String label, String help, String value, boolean on, Runnable onTap) {
            LinearLayout row = new LinearLayout(getContext());
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(18), dp(12), dp(18), dp(12));
            row.setBackground(rounded(0xFF1C2246, dp(14)));
            row.setMinimumHeight(dp(64));
            LinearLayout texts = new LinearLayout(getContext());
            texts.setOrientation(LinearLayout.VERTICAL);
            TextView title = new TextView(getContext());
            title.setText(label);
            title.setTextColor(Color.WHITE);
            title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            texts.addView(title);
            if (help != null) {
                TextView detail = new TextView(getContext());
                detail.setText(help);
                detail.setTextColor(0xFFA9B0D0);
                detail.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
                texts.addView(detail);
            }
            row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            TextView pill = new TextView(getContext());
            pill.setText(value);
            pill.setTextColor(on ? 0xFF10142A : Color.WHITE);
            pill.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
            pill.setTypeface(Typeface.DEFAULT_BOLD);
            pill.setPadding(dp(16), dp(8), dp(16), dp(8));
            pill.setBackground(rounded(on ? 0xFF7CE0A0 : 0xFF3A4270, dp(20)));
            row.addView(pill);
            row.setOnClickListener(v -> onTap.run());
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
            params.bottomMargin = dp(8);
            settings.addView(row, params);
        }

        private Button tabButton(String text) {
            Button button = new Button(getContext());
            button.setText(text);
            button.setAllCaps(false);
            button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            button.setFocusable(false);
            return button;
        }

        private void styleTab(Button tab, boolean selected) {
            tab.setTextColor(selected ? 0xFF10142A : Color.WHITE);
            tab.setBackground(rounded(selected ? 0xFFFFD34D : 0xFF242B55, dp(14)));
        }

        private GradientDrawable rounded(int color, int radius) {
            GradientDrawable drawable = new GradientDrawable();
            drawable.setColor(color);
            drawable.setCornerRadius(radius);
            return drawable;
        }

        private int dp(int value) {
            return Math.round(value * getContext().getResources().getDisplayMetrics().density);
        }
    }

    /** Top-down board: x to the right, z towards the bottom, as the default camera sees it. */
    private static final class BoardMapView extends View {
        private static final String[] NAMES = {"Mario", "Luigi", "Peach", "Yoshi", "Wario", "Daisy", "Waluigi", "DK"};
        private static final int[] COLORS = {0xFFE53935, 0xFF43A047, 0xFFF06292, 0xFF7CB342,
            0xFFFDD835, 0xFFFF9800, 0xFF8E24AA, 0xFF8D6E63};
        // Space types (src/game/board/space.c): 1 blue, 2 red, 3 Bowser, 4 mushroom, 5 battle,
        // 6 happening, 7 fortune, 8 star, 9 warp; 0 is a path point, not drawn.
        private static final int[] SPACE_COLORS = {0, 0xFF2F80ED, 0xFFE53935, 0xFF3D1A4F, 0xFFE8C21A,
            0xFFFB8C00, 0xFF2EAD5B, 0xFFEC6FB4, 0xFFFFD34D, 0xFF4FC3E8, 0xFF9E9E9E, 0xFF9E9E9E};

        // Tile of each space type in the game's 4x4 icon atlas (spaceImgIdx in space.c, 1-based).
        private static final int[] SPACE_TILES = {0, 1, 2, 7, 6, 5, 3, 4, 9, 10, 11, 0};

        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final Path star = new Path();
        private final Path clip = new Path();
        private final Rect src = new Rect();
        private final RectF dst = new RectF();
        private Bitmap atlas;
        private final Bitmap[] portraits = new Bitmap[8];
        private Bitmap coinIcon;
        private Bitmap starIcon;
        private float[] data = new float[0];
        private int size;
        private boolean live;

        BoardMapView(Context context) {
            super(context);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
        }

        void setImages(Bitmap[] images) {
            atlas = images[0];
            System.arraycopy(images, 1, portraits, 0, portraits.length);
            coinIcon = images[9];
            starIcon = images[10];
            invalidate();
        }

        // Draws the game's icon for a space type; false when there is none to draw.
        private boolean drawSpaceIcon(Canvas canvas, int type, float x, float y, float radius) {
            if (atlas == null || type >= SPACE_TILES.length || SPACE_TILES[type] <= 0) {
                return false;
            }
            int tile = SPACE_TILES[type] - 1;
            int cell = atlas.getWidth() / 4;
            int used = Math.round(atlas.getWidth() * 63f / 256f);
            src.set((tile % 4) * cell, (tile / 4) * cell, (tile % 4) * cell + used, (tile / 4) * cell + used);
            dst.set(x - radius, y - radius, x + radius, y + radius);
            paint.setColor(0xFF10142A); // hides the path under the icon's see-through rim
            canvas.drawCircle(x, y, radius * 0.9f, paint);
            paint.setAlpha(255);
            canvas.drawBitmap(atlas, src, dst, paint);
            return true;
        }

        // A round portrait from the game, ringed in the character's colour; initials without one.
        private void drawToken(Canvas canvas, int character, float x, float y, float radius) {
            Bitmap face = portraits[character];
            if (face == null) {
                paint.setColor(COLORS[character]);
                canvas.drawCircle(x, y, radius, paint);
                paint.setColor(Color.WHITE);
                paint.setTextAlign(Paint.Align.CENTER);
                paint.setTextSize(radius * 0.95f);
                String initial = character == 7 ? "DK" : character == 6 ? "Wa" : NAMES[character].substring(0, 1);
                canvas.drawText(initial, x, y + radius * 0.32f, paint);
                return;
            }
            paint.setColor(0xFF10142A);
            canvas.drawCircle(x, y, radius, paint);
            canvas.save();
            clip.reset();
            clip.addCircle(x, y, radius, Path.Direction.CW);
            canvas.clipPath(clip);
            // The status portrait stacks three square faces (normal, happy, sad): the first.
            src.set(0, 0, face.getWidth(), Math.min(face.getWidth(), face.getHeight()));
            dst.set(x - radius, y - radius, x + radius, y + radius);
            paint.setAlpha(255);
            canvas.drawBitmap(face, src, dst, paint);
            canvas.restore();
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(radius * 0.18f);
            paint.setColor(COLORS[character]);
            canvas.drawCircle(x, y, radius, paint);
            paint.setStyle(Paint.Style.FILL);
        }

        void update(float[] snapshot, int snapshotSize) {
            boolean onBoard = snapshotSize > HEADER && snapshot[1] != 0;
            live = onBoard;
            if (onBoard) {
                // Between turns (mini-games, results) the last board stays up, dimmed.
                if (data.length < snapshotSize) {
                    data = new float[snapshot.length];
                }
                System.arraycopy(snapshot, 0, data, 0, snapshotSize);
                size = snapshotSize;
            }
            invalidate();
        }

        private float sp(float value) {
            return value * getResources().getDisplayMetrics().scaledDensity;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float width = getWidth();
            float height = getHeight();
            paint.setStyle(Paint.Style.FILL);
            if (size <= HEADER) {
                paint.setColor(0xFFA9B0D0);
                paint.setTextSize(sp(20));
                paint.setTextAlign(Paint.Align.CENTER);
                canvas.drawText(tr("The map appears once a party is on the board.",
                    "La carte apparaît dès qu'une partie est sur le plateau."), width / 2, height / 2, paint);
                return;
            }
            int spaces = (int) data[6];
            int base = HEADER + 4 * PLAYER_STRIDE;
            float minX = Float.MAX_VALUE, maxX = -Float.MAX_VALUE, minZ = Float.MAX_VALUE, maxZ = -Float.MAX_VALUE;
            for (int i = 0; i < spaces; ++i) {
                float x = data[base + i * SPACE_STRIDE], z = data[base + i * SPACE_STRIDE + 1];
                minX = Math.min(minX, x); maxX = Math.max(maxX, x);
                minZ = Math.min(minZ, z); maxZ = Math.max(maxZ, z);
            }
            float panel = sp(104); // turn and scores, two rows
            float margin = sp(52); // room for the player tokens around the outer spaces
            float scale = Math.min((width - 2 * margin) / Math.max(1, maxX - minX),
                (height - panel - 2 * margin) / Math.max(1, maxZ - minZ));
            float offX = (width - (maxX - minX) * scale) / 2 - minX * scale;
            float offZ = panel + (height - panel - (maxZ - minZ) * scale) / 2 - minZ * scale;

            // Paths.
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(sp(4));
            paint.setColor(0x66FFFFFF);
            for (int i = 0; i < spaces; ++i) {
                int s = base + i * SPACE_STRIDE;
                for (int l = 0; l < 4; ++l) {
                    int target = (int) data[s + 3 + l];
                    if (target <= 0 || target > spaces) {
                        continue;
                    }
                    int t = base + (target - 1) * SPACE_STRIDE;
                    // Dead-end hidden points (star platform, item spots) are not part of the path.
                    if ((int) data[t + 2] == 0 && data[t + 3] == 0) {
                        continue;
                    }
                    canvas.drawLine(offX + data[s] * scale, offZ + data[s + 1] * scale,
                        offX + data[t] * scale, offZ + data[t + 1] * scale, paint);
                }
            }

            // Spaces.
            paint.setStyle(Paint.Style.FILL);
            // Spaces sit about 250 units apart: a radius of 115 leaves a little gap between them.
            float radius = Math.min(sp(14), Math.max(sp(6), 115 * scale));
            for (int i = 0; i < spaces; ++i) {
                int s = base + i * SPACE_STRIDE;
                int type = (int) data[s + 2];
                if (type <= 0 || type >= SPACE_COLORS.length) {
                    continue;
                }
                float x = offX + data[s] * scale, y = offZ + data[s + 1] * scale;
                if (type == 8) {
                    // The star waits here: the game's star space, under a big star.
                    drawSpaceIcon(canvas, type, x, y, radius);
                    drawStar(canvas, x, y, radius * 2.2f);
                } else if (!drawSpaceIcon(canvas, type, x, y, radius)) {
                    paint.setColor(SPACE_COLORS[type]);
                    canvas.drawCircle(x, y, radius, paint);
                }
            }

            // Players, the one whose turn it is ringed.
            int current = (int) data[4];
            for (int p = 0; p < 4; ++p) {
                int o = HEADER + p * PLAYER_STRIDE;
                int character = Math.max(0, Math.min(7, (int) data[o + 2]));
                // Each player keeps a corner, so four on one space stay readable.
                float x = offX + data[o] * scale + (p % 2 == 0 ? -1 : 1) * sp(9);
                float y = offZ + data[o + 1] * scale + (p < 2 ? -1 : 1) * sp(9);
                if (p == current) {
                    paint.setStyle(Paint.Style.STROKE);
                    paint.setStrokeWidth(sp(4));
                    paint.setColor(Color.WHITE);
                    canvas.drawCircle(x, y, sp(23), paint);
                    paint.setStyle(Paint.Style.FILL);
                }
                drawToken(canvas, character, x, y, sp(18));
            }

            // Turn and scores along the top.
            paint.setTextAlign(Paint.Align.LEFT);
            paint.setTextSize(sp(18));
            float column = (width - sp(150)) / 2; // two players a row, after the turn counter
            paint.setColor(Color.WHITE);
            canvas.drawText(tr("Turn ", "Tour ") + (int) data[2] + "/" + (int) data[3], sp(16), sp(36), paint);
            for (int p = 0; p < 4; ++p) {
                int o = HEADER + p * PLAYER_STRIDE;
                int character = Math.max(0, Math.min(7, (int) data[o + 2]));
                float x = sp(150) + column * (p % 2);
                float row = sp(30) + (p / 2) * sp(44);
                drawToken(canvas, character, x + sp(8), row, sp(16));
                paint.setTextAlign(Paint.Align.LEFT);
                paint.setTextSize(sp(20));
                paint.setColor(p == current ? 0xFFFFD34D : Color.WHITE);
                // The status bar's own star and coin icons; drawn symbols without them.
                float cursor = x + sp(32);
                cursor = drawCount(canvas, starIcon, "★", (int) data[o + 4], cursor, row);
                drawCount(canvas, coinIcon, "●", (int) data[o + 3], cursor + sp(14), row);
            }

            if (!live) {
                canvas.drawColor(0x99101428);
                paint.setColor(Color.WHITE);
                paint.setTextAlign(Paint.Align.CENTER);
                paint.setTextSize(sp(22));
                canvas.drawText(tr("Mini-game in progress", "Mini-jeu en cours"), width / 2, height / 2, paint);
            }
        }

        // An icon (or its stand-in symbol) and a count after it; returns where the text ends.
        private float drawCount(Canvas canvas, Bitmap icon, String symbol, int count, float x, float row) {
            String text = String.valueOf(count);
            if (icon == null) {
                text = symbol + text;
            } else {
                float h = sp(26);
                float w = h * icon.getWidth() / Math.max(1, icon.getHeight());
                src.set(0, 0, icon.getWidth(), icon.getHeight());
                dst.set(x, row - h / 2, x + w, row + h / 2);
                int color = paint.getColor();
                paint.setAlpha(255);
                canvas.drawBitmap(icon, src, dst, paint);
                paint.setColor(color);
                x += w + sp(3);
            }
            canvas.drawText(text, x, row + sp(7), paint);
            return x + paint.measureText(text);
        }

        private void drawStar(Canvas canvas, float cx, float cy, float r) {
            star.reset();
            for (int i = 0; i < 10; ++i) {
                double angle = Math.PI / 2 + i * Math.PI / 5;
                float radius = (i % 2 == 0) ? r : r * 0.45f;
                float x = cx + (float) (Math.cos(angle) * radius);
                float y = cy - (float) (Math.sin(angle) * radius);
                if (i == 0) {
                    star.moveTo(x, y);
                } else {
                    star.lineTo(x, y);
                }
            }
            star.close();
            paint.setColor(0xFFFFD34D);
            canvas.drawPath(star, paint);
        }
    }
}
