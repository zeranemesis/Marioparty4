package com.mariopartyrd.partyboard;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.util.Locale;

/**
 * Where CubeShelf for Android opens the game: the mods it installed are copied here first, off the
 * main thread and with their progress on screen (a texture pack can be hundreds of megabytes), then
 * the game starts with everything CubeShelf passed (RetroAchievements session, reply address).
 *
 * The game already running keeps its mods: nothing is replaced under it, and the new set applies
 * at its next start. An older CubeShelf opens PartyBoardActivity directly; CubeShelfMods then
 * copies at the game's start, as before.
 */
public class CubeShelfLaunchActivity extends Activity {
    private TextView status;
    private ProgressBar bar;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent incoming = getIntent();
        if (PartyBoardActivity.isRunning() || !CubeShelfMods.present(incoming)) {
            startGame(incoming, false);
            return;
        }

        boolean french = Locale.getDefault().getLanguage().equals("fr");
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(Color.rgb(5, 8, 17));
        int padding = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 32, getResources().getDisplayMetrics());
        layout.setPadding(padding, padding, padding, padding);
        status = new TextView(this);
        status.setTextColor(Color.rgb(248, 250, 255));
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        status.setGravity(Gravity.CENTER);
        status.setText(french ? "Préparation des mods…" : "Preparing mods…");
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        layout.addView(status);
        layout.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(layout);

        Thread copy = new Thread(() -> {
            boolean done = CubeShelfMods.sync(getApplicationContext(), incoming, (mod, mods, file, files) -> runOnUiThread(() -> {
                status.setText(french
                        ? String.format(Locale.FRANCE, "Préparation des mods… (%d/%d, %d/%d fichiers)", mod, mods, file, files)
                        : String.format(Locale.US, "Preparing mods… (%d/%d, %d/%d files)", mod, mods, file, files));
                bar.setProgress((int) (1000L * ((mod - 1) * Math.max(files, 1) + file) / ((long) mods * Math.max(files, 1))));
            }));
            runOnUiThread(() -> startGame(incoming, done));
        }, "CubeShelfMods");
        copy.start();
    }

    private void startGame(Intent incoming, boolean applied) {
        Intent game = new Intent(this, PartyBoardActivity.class);
        if (incoming != null && incoming.getExtras() != null) game.putExtras(incoming.getExtras());
        // Copied (or not, and the last set stays): the game only reads the list either way.
        game.putExtra(CubeShelfMods.EXTRA_APPLIED, true);
        startActivity(game);
        finish();
    }
}
