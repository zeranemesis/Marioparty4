package com.mariopartyrd.partyboard;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.Build;
import android.util.Log;

// Android's installer answers the session PartyBoardUpdater committed here: first with the screen
// that asks the player to confirm, then with the outcome. On success Android replaces the app and
// ends this process, so only failures need reporting.
public class InstallStatusReceiver extends BroadcastReceiver {
    private static final String TAG = "InstallStatusReceiver";

    @Override
    public void onReceive(Context context, Intent intent) {
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        switch (status) {
            case PackageInstaller.STATUS_PENDING_USER_ACTION: {
                Intent confirm = confirmationIntent(intent);
                if (confirm == null) {
                    PartyBoardUpdater.reportInstallFailure("the installer did not open");
                    return;
                }
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try {
                    context.startActivity(confirm);
                } catch (RuntimeException e) {
                    Log.w(TAG, "Could not open the installer", e);
                    PartyBoardUpdater.reportInstallFailure("the installer did not open");
                }
                return;
            }
            case PackageInstaller.STATUS_SUCCESS:
                return;
            case PackageInstaller.STATUS_FAILURE_ABORTED:
                PartyBoardUpdater.reportInstallFailure("cancelled");
                return;
            default: {
                String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                PartyBoardUpdater.reportInstallFailure(
                    message != null && !message.isEmpty() ? message : "status " + status);
            }
        }
    }

    @SuppressWarnings("deprecation")
    private static Intent confirmationIntent(Intent intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
        }
        return intent.getParcelableExtra(Intent.EXTRA_INTENT);
    }
}
