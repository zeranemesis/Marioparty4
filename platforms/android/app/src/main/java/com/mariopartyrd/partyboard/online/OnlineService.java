package com.mariopartyrd.partyboard.online;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.util.Log;

import com.mariopartyrd.partyboard.PartyBoardActivity;

import java.io.IOException;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

// Holds the lobby in the ":online" process, so it outlives the game process it
// restarts: the game reads its --netplay arguments once, at start, exactly like
// partyboard.exe launched by PartyBoardOnline.exe. A foreground service, so the
// relay keeps running while the game has the screen.
public final class OnlineService extends Service {
    private static final String TAG = "PartyBoardOnline";
    private static final String CHANNEL = "online";          // the game is starting: heads-up
    private static final String CHANNEL_ONGOING = "online_ongoing"; // the lobby is open: silent
    private static final int NOTIFICATION_ONGOING = 1;
    private static final int NOTIFICATION_START = 2;

    public static final String EXTRA_BARRIER = "partyboard_online_barrier";
    public static final String EXTRA_DISC = "partyboard_online_disc";
    public static final String EXTRA_LANGUAGE = "partyboard_online_language";

    interface Observer {
        void refresh();
    }

    final class LocalBinder extends Binder {
        OnlineService service() {
            return OnlineService.this;
        }
    }

    private final LocalBinder binder = new LocalBinder();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Observer> observers = new CopyOnWriteArrayList<>();

    volatile Session session;
    volatile DiscFile disc;
    volatile boolean hashing;
    volatile int hashProgress = -1;
    volatile String status = "";
    volatile String hashingName = "";
    volatile Report lastReport;
    volatile LobbyActivity visibleActivity;
    private volatile boolean cancelHash;
    private boolean foreground;
    private volatile Intent pendingGame;

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        cancelHash = true;
        Session s = session;
        session = null;
        if (s != null) {
            s.close();
        }
        super.onDestroy();
    }

    void addObserver(Observer o) {
        observers.add(o);
    }

    void removeObserver(Observer o) {
        observers.remove(o);
    }

    private void refresh() {
        main.post(() -> {
            for (Observer o : observers) {
                o.refresh();
            }
            updateOngoing();
        });
    }

    void setStatus(String text) {
        status = text == null ? "" : text;
        refresh();
    }

    // ---- Disc -----------------------------------------------------------------
    void verifyDisc(String path, Runnable onVerified) {
        if (session != null || hashing || path == null || path.isEmpty()) {
            return;
        }
        disc = null;
        hashing = true;
        cancelHash = false;
        hashProgress = 0;
        hashingName = DiscFile.displayName(this, path);
        setStatus(Msg.s("Vérification du disque…", "Verifying the disc…"));
        Thread t = new Thread(() -> {
            try {
                DiscFile verified = DiscFile.verify(this, path, pct -> {
                    hashProgress = pct;
                    refresh();
                }, () -> cancelHash);
                disc = verified;
                hashing = false;
                setStatus(Msg.s("Disque vérifié. Créez un salon ou collez l'invitation de votre ami.",
                    "Disc verified. Create a lobby or paste your friend's invitation."));
                if (onVerified != null) {
                    main.post(onVerified);
                }
            } catch (IOException | RuntimeException e) {
                hashing = false;
                setStatus(friendly(e));
            }
        }, "PartyBoard disc hash");
        t.setDaemon(true);
        t.start();
    }

    // ---- Session --------------------------------------------------------------
    void begin(boolean create, String invitation, int players, String nickname) {
        if (session != null || disc == null || hashing) {
            return;
        }
        Lobby.PlayerInfo profile;
        try {
            profile = new Lobby.PlayerInfo(nickname, disc.hash, disc.length, Lobby.ModSet.EMPTY);
        } catch (IOException e) {
            setStatus(e.getMessage());
            return;
        }
        startOngoing();
        final Session[] holder = new Session[1];
        Session.Listener listener = new Session.Listener() {
            @Override
            public void status(String text) {
                if (session == holder[0]) {
                    setStatus(text);
                }
            }

            @Override
            public void changed() {
                if (session == holder[0]) {
                    refresh();
                }
            }

            @Override
            public void failed(String text) {
                if (session == holder[0]) {
                    reset();
                    setStatus(text);
                }
            }
        };
        Session.GameLauncher launcher = new Session.GameLauncher() {
            @Override
            public void launch(String[] argv, String barrier, String discPath) throws IOException {
                launchGame(argv, barrier, discPath);
            }

            @Override
            public boolean gameAlive() {
                return gameProcessRunning();
            }
        };
        Session current = new Session(this, listener, launcher, profile, disc);
        holder[0] = current;
        current.host = create;
        session = current;
        lastReport = current.report;
        current.report.write("role=" + (create ? "host" : "guest") + " connection_requested");
        refresh();
        Thread t = new Thread(() -> {
            try {
                if (create) {
                    current.create(players);
                    if (session == current) {
                        setStatus(current.localOnly
                            ? Msg.s("Salon créé pour votre Wi-Fi : votre box refuse les connexions depuis Internet, vos amis doivent être sur le même réseau. Partagez l'invitation.",
                                "Lobby created for your Wi-Fi: your router refuses connections from the Internet, so your friends must be on the same network. Share the invitation.")
                            : Msg.s("Salon créé. Partagez l'invitation avec " + (players > 2 ? "vos amis" : "votre ami") + ". Vous seul pourrez lancer le jeu.",
                                "Lobby created. Share the invitation with your " + (players > 2 ? "friends" : "friend") + ". Only you can start the game."));
                    }
                } else {
                    current.join(invitation);
                }
            } catch (IOException | RuntimeException e) {
                current.close();
                if (session == current) {
                    reset();
                    setStatus(friendly(e));
                }
            }
        }, "PartyBoard begin");
        t.setDaemon(true);
        t.start();
    }

    void launch() {
        Session current = session;
        if (current == null) {
            return;
        }
        Thread t = new Thread(() -> {
            try {
                current.launch();
            } catch (IOException | RuntimeException e) {
                if (session == current) {
                    setStatus(friendly(e));
                }
            }
        }, "PartyBoard launch");
        t.setDaemon(true);
        t.start();
    }

    void reset() {
        Session old = session;
        session = null;
        if (old != null) {
            Thread t = new Thread(old::close, "PartyBoard close");
            t.setDaemon(true);
            t.start();
        }
        pendingGame = null;
        cancelStartNotification();
        setStatus(disc != null ? Msg.s("Créez un salon ou rejoignez votre ami.", "Create a lobby or join your friend.")
            : Msg.s("Choisissez votre disque pour commencer.", "Choose your disc to get started."));
        stopOngoing();
    }

    static String friendly(Throwable e) {
        if (e instanceof IOException && e.getMessage() != null && !e.getMessage().isEmpty()
            && !e.getClass().getName().startsWith("java.net") && !e.getClass().getName().startsWith("javax.net"))
        {
            return e.getMessage();
        }
        return Msg.s("L'opération n'a pas abouti. Vérifiez votre connexion ou recréez le salon.",
            "The operation did not complete. Check your connection or create the lobby again.");
    }

    // ---- The game process -----------------------------------------------------
    // A game that is still running (or an SDL process that finished its main
    // but lingers) would refuse to start a second main() in the same process.
    private void killGameProcess() {
        ActivityManager am = getSystemService(ActivityManager.class);
        if (am == null) {
            return;
        }
        List<ActivityManager.RunningAppProcessInfo> processes = am.getRunningAppProcesses();
        if (processes == null) {
            return;
        }
        for (ActivityManager.RunningAppProcessInfo p : processes) {
            if (p.processName.equals(getPackageName()) && p.pid != Process.myPid()) {
                Log.i(TAG, "Ending the previous game process before the online start");
                Process.killProcess(p.pid);
            }
        }
    }

    boolean gameProcessRunning() {
        ActivityManager am = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> processes = am == null ? null : am.getRunningAppProcesses();
        if (processes != null) {
            for (ActivityManager.RunningAppProcessInfo p : processes) {
                if (p.processName.equals(getPackageName())) {
                    return true;
                }
            }
        }
        return false;
    }

    static Intent gameIntent(Context context, String[] argv, String barrier, String discPath) {
        Intent intent = new Intent(context, PartyBoardActivity.class);
        intent.putExtra("partyboard_argv", argv);
        intent.putExtra(EXTRA_BARRIER, barrier);
        intent.putExtra(EXTRA_DISC, discPath);
        intent.putExtra(EXTRA_LANGUAGE, Msg.french() ? "fr" : "en");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        return intent;
    }

    private void launchGame(String[] argv, String barrier, String discPath) throws IOException {
        killGameProcess();
        Intent intent = gameIntent(this, argv, barrier, discPath);
        main.post(() -> {
            LobbyActivity activity = visibleActivity;
            if (activity != null) {
                activity.startActivity(intent);
                return;
            }
            pendingGame = intent;
            // Android lets only a visible app open a window. The guest may be in
            // a chat app pasting the invitation: ask them to come back, the host
            // waits up to two minutes for everyone.
            postStartNotification(intent);
        });
    }

    private void ensureChannel() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm == null) {
            return;
        }
        if (nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL,
                Msg.s("Début de partie", "Game start"), NotificationManager.IMPORTANCE_HIGH));
        }
        if (nm.getNotificationChannel(CHANNEL_ONGOING) == null) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL_ONGOING,
                Msg.s("Salon en ligne", "Online lobby"), NotificationManager.IMPORTANCE_LOW));
        }
    }

    private Notification buildOngoing() {
        Intent open = new Intent(this, LobbyActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pending = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Session s = session;
        Lobby l = s == null ? null : s.lobby;
        String text = l == null ? Msg.s("En attente des joueurs…", "Waiting for players…")
            : l.phase() == Lobby.Phase.RUNNING ? Msg.s("Partie en cours", "Game in progress")
            : Msg.s("Joueurs : ", "Players: ") + l.occupied() + "/" + s.maxPlayers();
        return new Notification.Builder(this, CHANNEL_ONGOING)
            .setSmallIcon(android.R.drawable.stat_sys_upload_done)
            .setContentTitle(Msg.s("Party Board — salon en ligne", "Party Board — online lobby"))
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(pending)
            .build();
    }

    private void startOngoing() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            main.post(this::startOngoing);
            return;
        }
        if (foreground) {
            return;
        }
        ensureChannel();
        startService(new Intent(this, OnlineService.class));
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ONGOING, buildOngoing(), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else {
                startForeground(NOTIFICATION_ONGOING, buildOngoing());
            }
            foreground = true;
        } catch (RuntimeException e) {
            Log.w(TAG, "Unable to keep the lobby in the foreground", e);
        }
    }

    private void updateOngoing() {
        if (!foreground || session == null) {
            return;
        }
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.notify(NOTIFICATION_ONGOING, buildOngoing());
        }
    }

    private void stopOngoing() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            main.post(this::stopOngoing);
            return;
        }
        if (!foreground) {
            return;
        }
        foreground = false;
        stopForeground(STOP_FOREGROUND_REMOVE);
        if (visibleActivity == null) {
            stopSelf();
        }
    }

    private void postStartNotification(Intent intent) {
        ensureChannel();
        PendingIntent pending = PendingIntent.getActivity(this, 1, intent,
            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(Msg.s("La partie commence !", "The game is starting!"))
            .setContentText(Msg.s("Touchez pour rejoindre vos amis.", "Tap to join your friends."))
            .setCategory(Notification.CATEGORY_CALL)
            .setAutoCancel(true)
            .setContentIntent(pending)
            .build();
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.notify(NOTIFICATION_START, n);
        }
    }

    private void cancelStartNotification() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.cancel(NOTIFICATION_START);
        }
    }

    void activityVisible(LobbyActivity activity) {
        visibleActivity = activity;
        cancelStartNotification();
        Intent game = pendingGame;
        pendingGame = null;
        if (game != null && session != null) {
            activity.startActivity(game);
        }
    }

    void activityHidden(LobbyActivity activity) {
        if (visibleActivity == activity) {
            visibleActivity = null;
        }
    }

    void activityGone() {
        if (session == null && !hashing) {
            stopSelf();
        }
    }
}
