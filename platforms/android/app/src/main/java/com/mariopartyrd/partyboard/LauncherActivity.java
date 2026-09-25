package com.mariopartyrd.partyboard;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.List;
import java.util.Locale;

// The app's entry point. Normally it starts the game at once and closes. When Android says the
// previous run crashed, or the game closed itself within a minute of starting, it first shows a
// report -- Android's exit reason and the end of the game's log (src/port/run_log.cpp) -- that the
// player can send, then starts the game again on request. Nothing leaves the phone unless the
// player shares it.
public class LauncherActivity extends Activity {
    private static final String PREFS = "partyboard_launcher";
    private static final String LAST_REPORTED = "last_reported_exit";
    private static final int LOG_TAIL_LINES = 250;
    private static final long QUICK_EXIT_MS = 60_000;

    private boolean mFrench;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mFrench = "fr".equals(Locale.getDefault().getLanguage());
        String report = null;
        try {
            report = buildReport();
        } catch (RuntimeException e) {
            // The report must never be the thing that keeps the game from starting.
        }
        if (report == null) {
            startGame();
            return;
        }
        showReport(report);
    }

    private String text(String english, String french) {
        return mFrench ? french : english;
    }

    static File logFile(Context context) {
        return new File(new File(context.getFilesDir(), "logs"), "last-run.log");
    }

    private void startGame() {
        Intent game = new Intent(this, PartyBoardActivity.class);
        Intent own = getIntent();
        if (own != null && own.getExtras() != null) {
            game.putExtras(own.getExtras());
        }
        startActivity(game);
        finish();
        overridePendingTransition(0, 0);
    }

    // Null when there is nothing to report.
    private String buildReport() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return null;
        }
        ActivityManager manager = getSystemService(ActivityManager.class);
        if (manager == null) {
            return null;
        }
        List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(null, 0, 1);
        if (exits == null || exits.isEmpty()) {
            return null;
        }
        ApplicationExitInfo exit = exits.get(0);
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (prefs.getLong(LAST_REPORTED, 0) == exit.getTimestamp()) {
            return null;
        }

        List<String> log = readLogTail();
        long started = 0;
        boolean clean = false;
        for (String line : log) {
            if (line.startsWith("partyboard-run-start ")) {
                try {
                    started = Long.parseLong(line.substring("partyboard-run-start ".length()).trim());
                } catch (NumberFormatException ignored) {
                }
            } else if (line.startsWith("partyboard-run-end")) {
                clean = true;
            }
        }
        if (!worthReporting(exit, started, clean)) {
            return null;
        }
        prefs.edit().putLong(LAST_REPORTED, exit.getTimestamp()).apply();

        StringBuilder report = new StringBuilder();
        report.append("Party Board crash report\n");
        report.append("App: ").append(getPackageName()).append(" build ").append(versionCode()).append('\n');
        report.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
            .append(", Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT)
            .append("), ").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?").append('\n');
        report.append("Exit: ").append(reasonName(exit.getReason())).append(", status ").append(exit.getStatus())
            .append(", at ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date(exit.getTimestamp())));
        if (started > 0) {
            report.append(", ").append((exit.getTimestamp() - started) / 1000).append(" s after start");
        }
        report.append('\n');
        if (exit.getDescription() != null) {
            report.append("Android says: ").append(exit.getDescription()).append('\n');
        }
        report.append("\n--- end of the game's log ---\n");
        if (log.isEmpty()) {
            report.append("(no log: the game stopped before writing one)\n");
        }
        for (String line : log) {
            report.append(line).append('\n');
        }
        return report.toString();
    }

    private static boolean worthReporting(ApplicationExitInfo exit, long started, boolean clean) {
        switch (exit.getReason()) {
            case ApplicationExitInfo.REASON_CRASH:
            case ApplicationExitInfo.REASON_CRASH_NATIVE:
            case ApplicationExitInfo.REASON_ANR:
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:
                return true;
            case ApplicationExitInfo.REASON_SIGNALED:
                // Fatal signals the crash dumper did not claim; plain kills are not crashes.
                int signal = exit.getStatus();
                return signal == 4 || signal == 6 || signal == 7 || signal == 8 || signal == 11;
            case ApplicationExitInfo.REASON_EXIT_SELF:
                if (exit.getStatus() != 0) {
                    return true;
                }
                // Closed by itself right after starting, without the game saying it quit.
                return !clean && started > 0 && exit.getTimestamp() - started < QUICK_EXIT_MS;
            default:
                return false;
        }
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_EXIT_SELF:
                return "EXIT_SELF";
            case ApplicationExitInfo.REASON_SIGNALED:
                return "SIGNALED";
            case ApplicationExitInfo.REASON_LOW_MEMORY:
                return "LOW_MEMORY";
            case ApplicationExitInfo.REASON_CRASH:
                return "CRASH (Java)";
            case ApplicationExitInfo.REASON_CRASH_NATIVE:
                return "CRASH_NATIVE";
            case ApplicationExitInfo.REASON_ANR:
                return "ANR (not responding)";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:
                return "INITIALIZATION_FAILURE";
            default:
                return "reason " + reason;
        }
    }

    @SuppressWarnings("deprecation")
    private long versionCode() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.P ? info.getLongVersionCode() : info.versionCode;
        } catch (PackageManager.NameNotFoundException e) {
            return 0;
        }
    }

    private List<String> readLogTail() {
        ArrayDeque<String> lines = new ArrayDeque<>();
        File file = logFile(this);
        if (!file.isFile()) {
            return new java.util.ArrayList<>(lines);
        }
        String header = null;
        try (BufferedReader reader = new BufferedReader(
                 new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (header == null) {
                    header = line;
                }
                lines.addLast(line);
                if (lines.size() > LOG_TAIL_LINES) {
                    lines.removeFirst();
                }
            }
        } catch (IOException ignored) {
        }
        java.util.ArrayList<String> result = new java.util.ArrayList<>(lines);
        // The start line carries the time the run began; keep it even when the tail cut it off.
        if (header != null && header.startsWith("partyboard-run-start ") && !result.isEmpty() && !result.get(0).equals(header)) {
            result.add(0, header);
        }
        return result;
    }

    private void showReport(String report) {
        int padding = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(24, 24, 32));
        root.setPadding(padding, padding, padding, padding);

        TextView title = new TextView(this);
        title.setText(text("Party Board closed unexpectedly", "Party Board s’est fermé de façon inattendue"));
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText(text("Send this report so the problem can be fixed. It contains the game's log and your phone model, nothing else.",
            "Envoie ce rapport pour que le problème soit corrigé. Il contient le journal du jeu et le modèle du téléphone, rien d’autre."));
        hint.setTextColor(Color.rgb(200, 200, 210));
        hint.setPadding(0, dp(8), 0, dp(8));
        root.addView(hint);

        ScrollView scroll = new ScrollView(this);
        TextView body = new TextView(this);
        body.setText(report);
        body.setTextColor(Color.rgb(220, 220, 220));
        body.setTypeface(Typeface.MONOSPACE);
        body.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        body.setTextIsSelectable(true);
        scroll.addView(body);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.END);
        buttons.setPadding(0, dp(8), 0, 0);

        Button share = new Button(this);
        share.setText(text("Send", "Envoyer"));
        share.setOnClickListener(v -> {
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_SUBJECT, "Party Board crash report");
            send.putExtra(Intent.EXTRA_TEXT, report);
            startActivity(Intent.createChooser(send, text("Send the report", "Envoyer le rapport")));
        });
        buttons.addView(share);

        Button copy = new Button(this);
        copy.setText(text("Copy", "Copier"));
        copy.setOnClickListener(v -> {
            ClipboardManager clipboard = getSystemService(ClipboardManager.class);
            if (clipboard != null) {
                clipboard.setPrimaryClip(ClipData.newPlainText("Party Board crash report", report));
                Toast.makeText(this, text("Report copied", "Rapport copié"), Toast.LENGTH_SHORT).show();
            }
        });
        buttons.addView(copy);

        Button restart = new Button(this);
        restart.setText(text("Start the game", "Relancer le jeu"));
        restart.setOnClickListener(v -> startGame());
        buttons.addView(restart);

        root.addView(buttons);
        setContentView(root);
    }

    private int dp(int value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics()));
    }
}
