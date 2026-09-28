package com.mariopartyrd.partyboard.online;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

import com.mariopartyrd.partyboard.PartyBoardActivity;

import java.util.List;

// Opens the game again from another process once the running one is gone: SDL never runs main()
// twice in one process, and saves received from CubeShelf are put in place at start.
public final class RestartActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            ActivityManager am = getSystemService(ActivityManager.class);
            List<ActivityManager.RunningAppProcessInfo> processes = am == null ? null : am.getRunningAppProcesses();
            if (processes != null) {
                for (ActivityManager.RunningAppProcessInfo p : processes) {
                    if (p.processName.equals(getPackageName()) && p.pid != Process.myPid()) {
                        Process.killProcess(p.pid);
                    }
                }
            }
            Intent game = new Intent(this, PartyBoardActivity.class);
            game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(game);
            finish();
        }, 700);
    }
}
