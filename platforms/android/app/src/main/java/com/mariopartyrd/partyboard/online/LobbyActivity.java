package com.mariopartyrd.partyboard.online;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.text.InputFilter;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.mariopartyrd.partyboard.PartyBoardActivity;

import java.net.InetAddress;

// tools/online/LobbyForm.cs, for a phone: nickname, disc, create or join, the
// invitation to share, who is in the salon with their disc and ping, and the
// start button only the host can press.
public final class LobbyActivity extends Activity implements OnlineService.Observer {
    public static final String EXTRA_NICKNAME = "nickname";
    public static final String EXTRA_DISC = "disc";
    public static final String EXTRA_LANGUAGE = "language";
    public static final String EXTRA_INVITATION = "invitation";
    private static final String EXTRA_TEST_PUBLIC = "online_test_public";
    private static final int PICK_DISC = 1;

    private static final int BACKGROUND = Color.rgb(0x10, 0x16, 0x24);
    private static final int CARD = Color.rgb(0x1b, 0x24, 0x38);
    private static final int ACCENT = Color.rgb(0x00, 0x9d, 0xda);
    private static final int ORANGE = Color.rgb(0xff, 0xa8, 0x26);
    private static final int TEXT = Color.rgb(0xf2, 0xf4, 0xf8);
    private static final int MUTED = Color.rgb(0xa6, 0xb0, 0xc3);
    private static final int GOOD = Color.rgb(0x7c, 0xd9, 0x92);
    private static final int BAD = Color.rgb(0xff, 0x7a, 0x7a);

    private OnlineService service;
    private SharedPreferences prefs;
    private EditText nickname;
    private EditText invitation;
    private TextView discLabel;
    private TextView status;
    private TextView footer;
    private LinearLayout players;
    private Button choose;
    private Button host;
    private Button join;
    private Button copy;
    private Button share;
    private Button paste;
    private Button play;
    private Button leave;
    private final Button[] counts = new Button[3];
    private int selectedPlayers = 2;
    private String pendingInvitation;
    private String initialDisc;
    private boolean autoJoin;

    private final ServiceConnection connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            service = ((OnlineService.LocalBinder) binder).service();
            service.addObserver(LobbyActivity.this);
            service.activityVisible(LobbyActivity.this);
            applyStartup();
            refresh();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            service = null;
        }
    };

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }

    private GradientDrawable rounded(int color, float radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        return d;
    }

    private TextView text(String value, float size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        t.setTextColor(color);
        if (bold) {
            t.setTypeface(Typeface.DEFAULT_BOLD);
        }
        return t;
    }

    private Button button(String label, int color, View.OnClickListener action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setBackground(rounded(color, 10));
        b.setPadding(dp(14), dp(8), dp(14), dp(8));
        b.setOnClickListener(v -> {
            try {
                action.onClick(v);
            } catch (RuntimeException e) {
                setStatus(OnlineService.friendly(e));
            }
        });
        return b;
    }

    private static void enable(View view, boolean enabled) {
        view.setEnabled(enabled);
        view.setAlpha(enabled ? 1f : 0.4f);
    }

    private LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        return r;
    }

    private LinearLayout.LayoutParams weighted(float weight, int marginEnd) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, weight);
        p.setMarginEnd(dp(marginEnd));
        return p;
    }

    private LinearLayout card(LinearLayout parent) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(rounded(CARD, 14));
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(12);
        parent.addView(c, p);
        return c;
    }

    private void space(LinearLayout parent, int height) {
        parent.addView(new View(this), new LinearLayout.LayoutParams(1, dp(height)));
    }

    private EditText field(String hint) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setTextColor(TEXT);
        e.setHintTextColor(MUTED);
        e.setBackground(rounded(Color.rgb(0x0c, 0x11, 0x1c), 8));
        e.setPadding(dp(12), dp(10), dp(12), dp(10));
        return e;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("online", MODE_PRIVATE);
        readIntent(getIntent());
        build();
        startService(new Intent(this, OnlineService.class));
        bindService(new Intent(this, OnlineService.class), connection, Context.BIND_AUTO_CREATE);
        if (Build.VERSION.SDK_INT >= 33
            && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
        {
            requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 2);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        readIntent(intent);
        if (service != null) {
            applyStartup();
        }
    }

    private void readIntent(Intent intent) {
        if (intent == null) {
            return;
        }
        Msg.setLanguage(intent.getStringExtra(EXTRA_LANGUAGE));
        if (intent.getStringExtra(EXTRA_LANGUAGE) != null) {
            prefs.edit().putString("language", intent.getStringExtra(EXTRA_LANGUAGE)).apply();
        } else {
            Msg.setLanguage(prefs.getString("language", null));
        }
        String name = intent.getStringExtra(EXTRA_NICKNAME);
        if (name != null && !name.trim().isEmpty() && !prefs.contains("nickname")) {
            prefs.edit().putString("nickname", name.trim()).apply();
        }
        String discPath = intent.getStringExtra(EXTRA_DISC);
        if (discPath != null && !discPath.isEmpty()) {
            initialDisc = discPath;
        }
        String shared = null;
        if (Intent.ACTION_SEND.equals(intent.getAction())) {
            shared = intent.getStringExtra(Intent.EXTRA_TEXT);
        } else if (intent.getStringExtra(EXTRA_INVITATION) != null) {
            shared = intent.getStringExtra(EXTRA_INVITATION);
        }
        if (shared != null && shared.contains("PB4.")) {
            pendingInvitation = shared;
            autoJoin = true;
        }
        boolean debuggable = (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        String test = intent.getStringExtra(EXTRA_TEST_PUBLIC);
        if (debuggable && test != null) {
            try {
                Session.testPublicAddress = test.isEmpty() ? null : InetAddress.getByName(test);
            } catch (Exception ignored) {
            }
        }
    }

    private void applyStartup() {
        if (service == null) {
            return;
        }
        if (pendingInvitation != null && service.session == null) {
            invitation.setText(Invitation.extract(pendingInvitation));
            pendingInvitation = null;
        }
        if (service.disc == null && !service.hashing) {
            String path = initialDisc != null ? initialDisc : prefs.getString("disc", null);
            initialDisc = null;
            if (path != null) {
                service.verifyDisc(path, this::runAutoJoin);
                return;
            }
            setStatus(autoJoin ? Msg.s("Invitation reçue. Choisissez votre disque : vous rejoindrez le salon dès qu'il sera vérifié.",
                "Invitation received. Choose your disc: you will join the lobby as soon as it is verified.")
                : Msg.s("Choisissez votre disque pour commencer.", "Choose your disc to get started."));
        } else {
            runAutoJoin();
        }
    }

    private void runAutoJoin() {
        if (!autoJoin || service == null || service.session != null || service.disc == null) {
            return;
        }
        autoJoin = false;
        begin(false);
    }

    private void build() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACKGROUND);
        scroll.setFillViewport(true);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(20), dp(20), dp(28));
        scroll.addView(root);
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            int top = 0, bottom = 0, left = 0, right = 0;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                top = bars.top;
                bottom = bars.bottom;
                left = bars.left;
                right = bars.right;
            }
            root.setPadding(dp(20) + left, dp(20) + top, dp(20) + right, dp(28) + bottom);
            return insets;
        });

        root.addView(text(Msg.s("Salon en ligne Party Board", "Party Board online lobby"), 26, TEXT, true));
        TextView intro = text(Msg.s("Choisissez votre pseudo et votre disque. L'hôte lancera le jeu pour tous.",
            "Choose your nickname and your disc. The host starts the game for everyone."), 14, MUTED, false);
        root.addView(intro);

        LinearLayout identity = card(root);
        identity.addView(text(Msg.s("Votre pseudo", "Your nickname"), 13, MUTED, false));
        nickname = field(Msg.s("Joueur", "Player"));
        nickname.setSingleLine(true);
        nickname.setFilters(new InputFilter[] { new InputFilter.LengthFilter(24) });
        nickname.setText(prefs.getString("nickname", Msg.s("Joueur", "Player")));
        identity.addView(nickname);
        space(identity, 12);
        identity.addView(text(Msg.s("Disque", "Disc"), 13, MUTED, false));
        LinearLayout discRow = row();
        discLabel = text("", 14, TEXT, false);
        discRow.addView(discLabel, weighted(1, 8));
        choose = button(Msg.s("Choisir", "Choose"), ACCENT, v -> pickDisc());
        discRow.addView(choose);
        identity.addView(discRow);

        LinearLayout session = card(root);
        session.addView(text(Msg.s("Joueurs", "Players"), 13, MUTED, false));
        LinearLayout countRow = row();
        for (int i = 0; i < 3; i++) {
            final int n = i + 2;
            counts[i] = button(String.valueOf(n), CARD, v -> {
                selectedPlayers = n;
                refresh();
            });
            countRow.addView(counts[i], weighted(1, i < 2 ? 8 : 0));
        }
        session.addView(countRow);
        space(session, 10);
        LinearLayout begin = row();
        host = button(Msg.s("Créer un salon", "Create a lobby"), ACCENT, v -> begin(true));
        join = button(Msg.s("Rejoindre", "Join"), ACCENT, v -> begin(false));
        begin.addView(host, weighted(1, 8));
        begin.addView(join, weighted(1, 0));
        session.addView(begin);
        space(session, 12);
        session.addView(text(Msg.s("Invitation", "Invitation"), 13, MUTED, false));
        invitation = field("PB4.…");
        invitation.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        invitation.setFilters(new InputFilter[] { new InputFilter.LengthFilter(220) });
        invitation.setTypeface(Typeface.MONOSPACE);
        invitation.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        session.addView(invitation);
        space(session, 8);
        LinearLayout actions = row();
        copy = button(Msg.s("Copier", "Copy"), CARD, v -> copyInvitation());
        share = button(Msg.s("Partager", "Share"), CARD, v -> shareInvitation());
        paste = button(Msg.s("Coller", "Paste"), CARD, v -> pasteInvitation());
        actions.addView(copy, weighted(1, 8));
        actions.addView(share, weighted(1, 8));
        actions.addView(paste, weighted(1, 0));
        session.addView(actions);

        LinearLayout list = card(root);
        list.addView(text(Msg.s("Dans le salon", "In the lobby"), 13, MUTED, false));
        players = new LinearLayout(this);
        players.setOrientation(LinearLayout.VERTICAL);
        list.addView(players);

        status = text("", 15, TEXT, false);
        status.setPadding(0, dp(14), 0, dp(4));
        root.addView(status);

        LinearLayout bottom = row();
        play = button(Msg.s("Lancer pour tout le monde", "Start for everyone"), ORANGE, v -> {
            if (service != null) {
                enable(play, false);
                service.launch();
            }
        });
        leave = button(Msg.s("Quitter le salon", "Leave the lobby"), CARD, v -> {
            if (service != null) {
                service.reset();
            }
        });
        bottom.addView(play, weighted(3, 8));
        bottom.addView(leave, weighted(2, 0));
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        bp.topMargin = dp(8);
        root.addView(bottom, bp);

        LinearLayout extra = row();
        Button report = button(Msg.s("Exporter le diagnostic", "Export diagnostic"), CARD, v -> exportReport());
        Button back = button(Msg.s("Retour au jeu", "Back to the game"), CARD, v -> backToGame());
        extra.addView(report, weighted(1, 8));
        extra.addView(back, weighted(1, 0));
        LinearLayout.LayoutParams ep = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        ep.topMargin = dp(10);
        root.addView(extra, ep);

        footer = text("", 12, MUTED, false);
        footer.setPadding(0, dp(12), 0, 0);
        root.addView(footer);

        setContentView(scroll);
    }

    private void setStatus(String text) {
        if (service != null) {
            service.setStatus(text);
        } else if (status != null) {
            status.setText(text);
        }
    }

    private void pickDisc() {
        if (service == null || service.session != null || service.hashing) {
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(pick, PICK_DISC);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_DISC || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
        String path = uri.toString();
        prefs.edit().putString("disc", path).apply();
        if (service != null) {
            service.verifyDisc(path, this::runAutoJoin);
        }
    }

    private void begin(boolean create) {
        if (service == null) {
            return;
        }
        String name = nickname.getText().toString().trim();
        try {
            name = Lobby.PlayerInfo.cleanName(name);
        } catch (java.io.IOException e) {
            setStatus(e.getMessage());
            return;
        }
        prefs.edit().putString("nickname", name).apply();
        if (!create && invitation.getText().toString().trim().isEmpty()) {
            setStatus(Msg.s("Collez d'abord l'invitation de votre ami.", "Paste your friend's invitation first."));
            return;
        }
        service.begin(create, invitation.getText().toString(), selectedPlayers, name);
    }

    private String currentInvitation() {
        Session s = service == null ? null : service.session;
        if (s == null || s.invite == null || !s.host) {
            return null;
        }
        try {
            return s.invite.encode();
        } catch (java.io.IOException e) {
            return null;
        }
    }

    private void copyInvitation() {
        String code = currentInvitation();
        if (code == null) {
            return;
        }
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText("Party Board", code));
        }
        setStatus(Msg.s("Invitation copiée. Envoyez-la à votre ami ; gardez le salon ouvert.",
            "Invitation copied. Send it to your friend; keep the lobby open."));
    }

    private void shareInvitation() {
        String code = currentInvitation();
        if (code == null) {
            return;
        }
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_TEXT, Msg.s("Rejoins mon salon Mario Party 4 sur Party Board (Jouer en ligne > Coller) : ",
            "Join my Mario Party 4 lobby in Party Board (Play Online > Paste): ") + code);
        startActivity(Intent.createChooser(send, Msg.s("Partager l'invitation", "Share the invitation")));
    }

    private void pasteInvitation() {
        if (service != null && service.session != null) {
            return;
        }
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm == null || !cm.hasPrimaryClip() || cm.getPrimaryClip().getItemCount() == 0) {
            return;
        }
        CharSequence clip = cm.getPrimaryClip().getItemAt(0).coerceToText(this);
        if (clip == null) {
            return;
        }
        invitation.setText(Invitation.extract(clip.toString()));
    }

    private void exportReport() {
        String content;
        try {
            Report r = service == null ? null : service.lastReport;
            content = r != null ? r.read() : Report.readLatest(this);
        } catch (java.io.IOException e) {
            setStatus(e.getMessage());
            return;
        }
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_SUBJECT, "Diagnostic-PartyBoard.txt");
        send.putExtra(Intent.EXTRA_TEXT, content);
        startActivity(Intent.createChooser(send, Msg.s("Exporter le diagnostic", "Export diagnostic")));
    }

    private void backToGame() {
        if (service != null && service.session != null) {
            Lobby l = service.session.lobby;
            if (l == null || l.phase() != Lobby.Phase.RUNNING) {
                service.reset();
            }
        }
        Intent game = new Intent(this, PartyBoardActivity.class);
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(game);
        finish();
    }

    @Override
    public void refresh() {
        if (service == null || status == null) {
            return;
        }
        Session s = service.session;
        boolean idle = s == null;
        boolean hashing = service.hashing;
        DiscFile disc = service.disc;
        Lobby lobby = s == null ? null : s.lobby;

        if (hashing) {
            discLabel.setText(service.hashingName + "\n" + Msg.s("Vérification SHA-256 : ", "SHA-256 check: ")
                + Math.max(0, service.hashProgress) + " %");
        } else if (disc != null) {
            discLabel.setText(disc.displayName + "\n" + Msg.s("Fichier vérifié", "File verified"));
        } else {
            discLabel.setText(Msg.s("Mario Party 4 USA, révision 1\nISO, GCM, RVZ, WIA ou CISO", "Mario Party 4 USA, revision 1\nISO, GCM, RVZ, WIA or CISO"));
        }
        enable(choose, idle && !hashing);
        nickname.setEnabled(idle && !hashing);
        enable(host, idle && disc != null && !hashing);
        enable(join, idle && disc != null && !hashing);
        enable(paste, idle);
        invitation.setEnabled(idle);
        for (int i = 0; i < counts.length; i++) {
            enable(counts[i], idle);
            counts[i].setBackground(rounded(selectedPlayers == i + 2 ? ACCENT : Color.rgb(0x2a, 0x35, 0x4d), 10));
        }
        boolean canInvite = s != null && s.invite != null && s.host
            && (lobby == null ? s.bridge == null : lobby.occupied() < s.maxPlayers());
        enable(copy, canInvite);
        enable(share, canInvite);
        if (canInvite) {
            String code = currentInvitation();
            if (code != null && !code.contentEquals(invitation.getText())) {
                invitation.setText(code);
            }
        }
        enable(leave, !idle);
        enable(play, lobby != null && lobby.canStart() && s.fastChannelReady());
        play.setText(s != null && !s.host ? Msg.s("L'hôte lance la partie", "The host starts the game")
            : Msg.s("Lancer pour tout le monde", "Start for everyone"));

        players.removeAllViews();
        String localName = lobby != null && lobby.local() != null ? lobby.local().name : nickname.getText().toString();
        boolean localMatch = lobby != null && lobby.seatAgrees(lobby.localSeat());
        String localRole = s == null ? Msg.s("Vous", "You") : s.host ? Msg.s("Hôte (vous)", "Host (you)") : Msg.s("Invité (vous)", "Guest (you)");
        String localDisc = disc == null ? Msg.s("À choisir", "To choose") : localMatch ? Msg.s("Identique — vérifié", "Identical — verified")
            : Msg.s("Vérifié — en attente", "Verified — waiting");
        addPlayer(localName, localRole, localDisc, Msg.s("Local", "Local"), localMatch ? GOOD : TEXT);
        boolean anyRemote = false;
        if (lobby != null) {
            for (int seat = 0; seat < Lobby.MAX_SEATS; seat++) {
                if (seat == lobby.localSeat()) {
                    continue;
                }
                Lobby.PlayerInfo info = lobby.seatInfo(seat);
                if (info == null) {
                    continue;
                }
                anyRemote = true;
                boolean agrees = lobby.seatAgrees(seat);
                String seatDisc = agrees ? Msg.s("Identique — vérifié", "Identical — verified")
                    : info.discHash == null ? Msg.s("Non vérifié", "Not verified") : Msg.s("DISQUE DIFFÉRENT", "DIFFERENT DISC");
                String ping;
                if (s.maxPlayers() > 2) {
                    ping = "—";
                } else {
                    Bridge b = s.bridge;
                    ping = b != null && b.udpReady() && s.pingMs != null ? s.pingMs + " ms" : Msg.s("En attente…", "Waiting…");
                }
                addPlayer(info.name, seat == 0 ? Msg.s("Hôte", "Host") : Msg.s("Invité", "Guest"), seatDisc, ping, agrees ? GOOD : BAD);
            }
        }

        String text = service.status;
        if (lobby != null) {
            if (lobby.ending() == Lobby.Ending.REMOTE_GAME_CLOSED) {
                text = Msg.s("Votre ami a quitté la partie. Le salon est fermé — recréez-en un pour rejouer.",
                    "Your friend left the game. The lobby is closed — create a new one to play again.");
            } else if (lobby.phase() == Lobby.Phase.PREPARING) {
                text = Msg.s("Chargement sur tous les téléphones… Le jeu attendra que tout le monde soit prêt.",
                    "Loading on every phone… The game waits until everyone is ready.");
            } else if (lobby.phase() == Lobby.Phase.RUNNING) {
                Bridge b = s.bridge;
                text = b == null || b.controlConnected() ? Msg.s("Partie lancée par l'hôte. Gardez le salon ouvert pendant le jeu.",
                    "Game started by the host. Keep the lobby open while playing.")
                    : Msg.s("Le canal du salon est interrompu. La partie continue tant que l'autre joueur reste joignable.",
                        "The lobby channel dropped. The game goes on while the other player stays reachable.");
            } else if (anyRemote) {
                if (!localMatch) {
                    text = Msg.s("Les fichiers disques sont différents : lancement bloqué. Choisissez exactement le même fichier sur tous les téléphones, puis recréez le salon.",
                        "The disc files differ: start blocked. Choose exactly the same file on every phone, then create the lobby again.");
                } else if (!lobby.modsMatch()) {
                    text = Msg.s("Les mods ne correspondent pas : ", "The mods do not match: ") + lobby.modAdvice();
                } else if (s.host && !s.fastChannelReady()) {
                    text = Msg.s("Disques identiques. Préparation du canal rapide du jeu…", "Identical discs. Preparing the game's fast channel…");
                } else {
                    text = s.host ? Msg.s("Disques identiques. Vous pouvez lancer la partie pour tout le monde.",
                        "Identical discs. You can start the game for everyone.")
                        : Msg.s("Disques identiques. Attendez que l'hôte lance la partie.", "Identical discs. Wait for the host to start the game.");
                }
            }
        }
        status.setText(text);
        String route = s != null && s.localOnly ? Msg.s(" · Même Wi-Fi uniquement", " · Same Wi-Fi only") : "";
        footer.setText(selectedPlayers + Msg.s(" joueurs", " players") + (selectedPlayers > 2 ? Msg.s(" (expérimental)", " (experimental)") : "")
            + Msg.s(" · Même fichier disque requis · Ping : aller-retour entre les téléphones", " · Same disc file required · Ping: round trip between the phones")
            + route);
    }

    private void addPlayer(String name, String role, String disc, String ping, int discColor) {
        LinearLayout r = row();
        r.setPadding(0, dp(8), 0, dp(8));
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.addView(text(name, 16, TEXT, true));
        left.addView(text(role, 13, MUTED, false));
        r.addView(left, weighted(1, 8));
        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setGravity(Gravity.END);
        TextView d = text(disc, 13, discColor, false);
        d.setGravity(Gravity.END);
        right.addView(d);
        TextView p = text(ping, 13, MUTED, false);
        p.setGravity(Gravity.END);
        right.addView(p);
        r.addView(right, weighted(1, 0));
        players.addView(r);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (service != null) {
            service.activityVisible(this);
            refresh();
        }
    }

    @Override
    protected void onPause() {
        if (service != null) {
            service.activityHidden(this);
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (service != null) {
            service.removeObserver(this);
            service.activityHidden(this);
            service.activityGone();
        }
        try {
            unbindService(connection);
        } catch (IllegalArgumentException ignored) {
        }
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        // Leaving this screen must not silently close a salon others are in:
        // the lobby stays in the notification, and Back returns to the game.
        if (service != null && service.session != null) {
            moveTaskToBack(true);
            Toast.makeText(this, Msg.s("Le salon reste ouvert en arrière-plan.", "The lobby stays open in the background."),
                Toast.LENGTH_SHORT).show();
            return;
        }
        backToGame();
    }
}
