package com.mariopartyrd.partyboard.online;

import android.content.Context;
import android.content.pm.ApplicationInfo;

import java.io.IOException;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.channels.DatagramChannel;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

import javax.net.ssl.SSLSocket;

// tools/online/Program.cs (Session): one lobby, from its creation or the join
// to the end of the game it launched. The game itself runs in its own process,
// exactly like partyboard.exe next to PartyBoardOnline.exe: it is started with
// the --netplay arguments and reaches the other phones through this relay.
final class Session implements AutoCloseable {
    interface Listener {
        void status(String text);

        void changed();

        void failed(String text);
    }

    interface GameLauncher {
        void launch(String[] argv, String barrier, String discPath) throws IOException;

        boolean gameAlive();
    }

    private final Context context;
    private final Listener listener;
    private final GameLauncher launcher;
    final Lobby.PlayerInfo profile;
    private final DiscFile disc;
    final Report report;

    volatile Invitation invite;
    volatile Bridge bridge;
    volatile boolean host;
    volatile Lobby lobby;
    volatile Integer pingMs;
    volatile String joinPath = "listening";
    volatile boolean localOnly;

    private ServerSocket server;
    private Wire.Identity cert;
    private Gateway mapping;
    private Gateway mappingUdp;
    private DatagramChannel internetGame;
    private Gateway meshMapping;
    private MeshRelay meshRelay;
    private int maxPlayers = 2;
    private Bridge[] guestLinks;
    private byte[] build;
    private int hostGamePort;
    private GameLink gameLink;
    private ScheduledExecutorService diagnosticTimer;
    private Gateway.Route route;
    private volatile boolean disposed;

    // Debug builds only: lets two emulators on one PC play through the host
    // machine's port forwarding, which no router mapping can describe.
    static volatile InetAddress testPublicAddress;

    Session(Context context, Listener listener, GameLauncher launcher, Lobby.PlayerInfo profile, DiscFile disc) {
        this.context = context.getApplicationContext();
        this.listener = listener;
        this.launcher = launcher;
        this.profile = profile;
        this.disc = disc;
        report = new Report(this.context);
    }

    int maxPlayers() {
        return maxPlayers;
    }

    boolean disposed() {
        return disposed;
    }

    private boolean debuggable() {
        return (context.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    private static void spawn(String name, Runnable body) {
        Thread t = new Thread(body, "PartyBoard " + name);
        t.setDaemon(true);
        t.start();
    }

    private void checkDisposed() throws IOException {
        if (disposed) {
            throw new IOException(Msg.s("Salon fermé.", "Lobby closed."));
        }
    }

    // players == 2 is the classic pair: one TCP+UDP port mapped on the box and
    // a single Bridge. Above two, every seat reaches the others through the
    // mesh, and the host only relays the salon's control messages.
    void create(int players) throws IOException {
        if (players < 2 || players > Lobby.MAX_SEATS) {
            throw new IOException(Msg.s("Nombre de joueurs invalide.", "Invalid number of players."));
        }
        maxPlayers = players;
        host = true;
        listener.status(Msg.s("Vérification de votre connexion…", "Checking your connection…"));
        route = Gateway.Route.detect(context);
        build = Wire.buildHash(context);
        cert = Wire.certificate();
        InetAddress test = debuggable() ? testPublicAddress : null;
        // An emulator's port forwarding reaches its other interface.
        InetAddress bindAddress = test != null ? Invitation.any() : route.local;
        server = new ServerSocket();
        server.bind(new InetSocketAddress(bindAddress, 0), 4);
        int port = server.getLocalPort();
        try {
            if (players == 2) {
                internetGame = DatagramChannel.open();
                internetGame.bind(new InetSocketAddress(bindAddress, port));
            }
            checkDisposed();
            InetAddress publicAddress;
            int publicPort;
            if (test != null) {
                publicAddress = test;
                publicPort = port;
                report.write("mapping=test");
            } else {
                listener.status(Msg.s("Préparation automatique de votre box…", "Setting up your router automatically…"));
                try {
                    mapping = new Gateway(context, route, port, port, false);
                    mapping.open();
                    if (players == 2) {
                        mappingUdp = new Gateway(context, route, port, mapping.port(), true);
                        mappingUdp.open();
                        if (mappingUdp.port() != mapping.port() || !mappingUdp.address.equals(mapping.address)) {
                            throw new IOException("TCP and UDP mappings differ");
                        }
                    }
                    publicAddress = mapping.address;
                    publicPort = mapping.port();
                    report.write("mapping=ok");
                } catch (IOException e) {
                    // The box said no. Friends on the same Wi-Fi can still come in:
                    // the invitation then carries only this phone's local address.
                    if (mappingUdp != null) {
                        mappingUdp.close();
                        mappingUdp = null;
                    }
                    if (mapping != null) {
                        mapping.close();
                        mapping = null;
                    }
                    if (!Gateway.isPrivate(route.local)) {
                        throw e;
                    }
                    localOnly = true;
                    publicAddress = Invitation.any();
                    publicPort = 0;
                    report.write("mapping=failed lan_only=true");
                }
            }
            checkDisposed();
            Invitation i = new Invitation();
            i.address = publicAddress;
            i.port = publicPort;
            i.expiresSeconds = System.currentTimeMillis() / 1000L + 30 * 60;
            i.fingerprint = cert.fingerprint;
            i.token = Wire.random(16);
            i.build = build;
            i.maxPlayers = players;
            boolean privateLocal = Gateway.isPrivate(route.local);
            i.localAddress = privateLocal ? route.local : Invitation.any();
            i.localPort = privateLocal ? port : 0;
            invite = i;
            if (players > 2) {
                guestLinks = new Bridge[players];
                lobby = new Lobby(true, profile, this::sendToSeat, this::prepare, this::commit, listener::changed);
                report.write("role=host mode=mesh build=" + Wire.hex(build) + " players=" + players);
                openMesh();
                startDiagnostics();
                lobby.announce();
                listener.changed();
            }
            spawn("maintain", this::maintain);
            spawn("accept", this::accept);
        } catch (IOException | RuntimeException e) {
            close();
            throw e;
        }
    }

    private void sendToSeat(int seat, byte[] payload) {
        Bridge[] links = guestLinks;
        if (links == null) {
            return;
        }
        if (seat == Lobby.BROADCAST) {
            for (int s = 1; s < links.length; s++) {
                Bridge link = links[s];
                if (link != null) {
                    try {
                        link.sendControl(payload);
                    } catch (IOException ignored) {
                    }
                }
            }
        } else if (seat >= 1 && seat < links.length && links[seat] != null) {
            try {
                links[seat].sendControl(payload);
            } catch (IOException ignored) {
            }
        }
    }

    private void maintain() {
        try {
            while (!disposed) {
                long wait = mapping == null ? 5 : Math.max(5, mapping.lifetime / 2);
                Thread.sleep(wait * 1000L);
                if (disposed) {
                    return;
                }
                if (bridge == null && System.currentTimeMillis() > invite.expiresMillis() && maxPlayers == 2) {
                    listener.failed(Msg.s("L'invitation a expiré. Créez un nouveau salon pour recommencer.",
                        "The invitation has expired. Create a new lobby to start again."));
                    close();
                    return;
                }
                synchronized (this) {
                    if (disposed) {
                        return;
                    }
                    if (mapping != null) {
                        mapping.renew();
                    }
                    if (mappingUdp != null) {
                        mappingUdp.renew();
                        if (mappingUdp.port() != mapping.port() || !mappingUdp.address.equals(mapping.address)) {
                            throw new IOException(Msg.s("La connexion de la box a changé.", "The router connection changed."));
                        }
                    }
                }
            }
        } catch (InterruptedException ignored) {
        } catch (IOException e) {
            if (!disposed) {
                listener.failed(Msg.s("La connexion temporaire a été interrompue. Recréez un salon.",
                    "The temporary connection was interrupted. Create the lobby again."));
                close();
            }
        }
    }

    private static final class PendingGuest {
        final SSLSocket[] channels = new SSLSocket[2];
    }

    private void accept() {
        try {
            int attempts = 0;
            long windowStart = System.currentTimeMillis();
            Map<String, PendingGuest> pending = new HashMap<>();
            int seated = 0;
            server.setSoTimeout(1000);
            while (!disposed) {
                if (System.currentTimeMillis() - windowStart >= 60_000) {
                    attempts = 0;
                    windowStart = System.currentTimeMillis();
                }
                if (attempts >= 6) {
                    Thread.sleep(1000);
                    continue;
                }
                Socket client;
                try {
                    client = server.accept();
                } catch (SocketTimeoutException e) {
                    continue;
                }
                attempts++;
                Wire.Accepted accepted;
                try {
                    accepted = Wire.server(client, cert, invite, build);
                } catch (IOException e) {
                    Wire.closeQuietly(client);
                    if (!disposed) {
                        listener.status(Msg.s("En attente de votre ami…", "Waiting for your friend…"));
                    }
                    continue;
                }
                if (disposed) {
                    Wire.closeQuietly(accepted.ssl);
                    return;
                }
                // Both channels of a guest arrive independently and in any order;
                // the client id pairs them. A channel arriving twice is refused
                // rather than allowed to steal the first one's slot.
                String key = Wire.hex(accepted.clientId);
                PendingGuest guest = pending.get(key);
                if (guest == null) {
                    if (maxPlayers == 2 && !pending.isEmpty()) {
                        Wire.closeQuietly(accepted.ssl); // one salon, one guest
                        continue;
                    }
                    guest = new PendingGuest();
                    pending.put(key, guest);
                }
                if (guest.channels[accepted.channel] != null) {
                    Wire.closeQuietly(accepted.ssl);
                    continue;
                }
                guest.channels[accepted.channel] = accepted.ssl;
                if (guest.channels[0] == null || guest.channels[1] == null) {
                    listener.status(Msg.s("Premier canal sécurisé. Préparation du second…", "First secure channel ready. Preparing the second…"));
                    continue;
                }
                pending.remove(key);
                if (maxPlayers == 2) {
                    Wire.closeQuietly(server);
                    int gamePort;
                    try (DatagramSocket reserve = new DatagramSocket(new InetSocketAddress(Wire.LOOPBACK, 0))) {
                        gamePort = reserve.getLocalPort();
                    }
                    hostGamePort = gamePort;
                    joinPath = "host";
                    bridge = new Bridge(guest.channels[0], guest.channels[1], 0, internetGame, null, invite.token, gamePort, -1, false);
                    internetGame = null;
                    attachLobby();
                    return;
                }
                int seat = 0;
                for (int s = 1; s < maxPlayers; s++) {
                    if (guestLinks[s] == null) {
                        seat = s;
                        break;
                    }
                }
                if (seat == 0) {
                    Wire.closeQuietly(guest.channels[0]);
                    Wire.closeQuietly(guest.channels[1]);
                    continue;
                }
                Bridge link = new Bridge(guest.channels[0], guest.channels[1], 0, null, null, invite.token, 0, seat, true);
                link.diagnostic = report::write;
                guestLinks[seat] = link;
                final int admitted = seat;
                link.control = packet -> lobby.receive(admitted, packet);
                lobby.admit(seat);
                watchGuest(seat, link);
                seated++;
                if (seated >= maxPlayers - 1) {
                    listener.status(Msg.s("Salon complet.", "The lobby is full."));
                    Wire.closeQuietly(server);
                    return;
                }
                listener.status(Msg.s("En attente d'un autre invité…", "Waiting for another guest…"));
            }
        } catch (InterruptedException ignored) {
        } catch (IOException | RuntimeException e) {
            if (!disposed) {
                listener.failed(Msg.s("L'attente a été interrompue. Recréez un salon.", "Waiting was interrupted. Create the lobby again."));
                close();
            }
        }
    }

    private void watchGuest(int seat, Bridge link) {
        spawn("guest " + seat, () -> {
            try {
                link.run();
            } catch (IOException e) {
                report.write("guest_seat=" + seat + " transport_error=" + e.getClass().getSimpleName());
            } finally {
                synchronized (this) {
                    if (!disposed && guestLinks != null && seat < guestLinks.length && guestLinks[seat] == link) {
                        guestLinks[seat] = null;
                    }
                }
                if (!disposed && lobby != null) {
                    lobby.leave(seat);
                }
                link.close();
            }
        });
    }

    void join(String invitationText) throws IOException {
        host = false;
        boolean test = debuggable() && testPublicAddress != null;
        invite = Invitation.decode(invitationText, test);
        maxPlayers = invite.maxPlayers;
        build = Wire.buildHash(context);
        listener.status(Msg.s("Connexion à votre ami…", "Connecting to your friend…"));
        byte[] clientId = Wire.random(16);
        InetAddress target = invite.address;
        int targetPort = invite.port;
        SSLSocket toHost = null;
        IOException lanError = null;
        // The host's own network first: two seconds, because an address that is
        // not on this network refuses or times out fast. A stranger at that
        // address fails the pinned handshake, exactly as if nobody answered.
        if (invite.hasLocalPath()) {
            listener.status(Msg.s("Recherche de votre ami sur votre réseau…", "Looking for your friend on your network…"));
            Socket lan = new Socket();
            try {
                lan.connect(new InetSocketAddress(invite.localAddress, invite.localPort), 2000);
                toHost = Wire.client(lan, invite, build, 0, clientId);
                target = invite.localAddress;
                targetPort = invite.localPort;
                joinPath = "lan";
            } catch (IOException e) {
                lanError = e;
                Wire.closeQuietly(lan);
            }
        }
        if (toHost == null) {
            if (lanError instanceof Wire.Refused) {
                throw lanError;
            }
            if (invite.localOnly()) {
                throw new IOException(Msg.s(
                    "Votre ami n'est pas joignable. Sa box refuse les connexions depuis Internet : connectez-vous au même Wi-Fi que lui, puis réessayez.",
                    "Your friend cannot be reached. Their router refuses connections from the Internet: join the same Wi-Fi as them, then try again."));
            }
            listener.status(Msg.s("Connexion à votre ami…", "Connecting to your friend…"));
            Socket peer = new Socket();
            try {
                peer.connect(new InetSocketAddress(invite.address, invite.port), 10000);
            } catch (IOException e) {
                Wire.closeQuietly(peer);
                throw new IOException(Msg.s(
                    "Votre ami n'est pas joignable. Vérifiez qu'il a laissé son salon ouvert, ou inversez les rôles.",
                    "Your friend cannot be reached. Check that their lobby is still open, or swap roles."));
            }
            toHost = Wire.client(peer, invite, build, 0, clientId);
            target = invite.address;
            targetPort = invite.port;
            joinPath = "internet";
        }
        listener.status(Msg.s("Premier canal sécurisé. Préparation du second…", "First secure channel ready. Preparing the second…"));
        Socket second = new Socket();
        SSLSocket fromHost;
        try {
            second.connect(new InetSocketAddress(target, targetPort), 10000);
            fromHost = Wire.client(second, invite, build, 1, clientId);
        } catch (IOException e) {
            Wire.closeQuietly(second);
            Wire.closeQuietly(toHost);
            throw e instanceof Wire.Refused ? e
                : new IOException(Msg.s("Le second canal n'a pas pu être créé.", "The second channel could not be opened."), e);
        }
        if (disposed) {
            Wire.closeQuietly(toHost);
            Wire.closeQuietly(fromHost);
            throw new IOException(Msg.s("Salon fermé.", "Lobby closed."));
        }
        DatagramChannel udp = DatagramChannel.open();
        udp.bind(new InetSocketAddress(0));
        bridge = new Bridge(fromHost, toHost, 1, udp, new InetSocketAddress(target, targetPort), invite.token, 0, -1,
            maxPlayers > 2);
        attachLobby();
        if (maxPlayers > 2) {
            // The seat is only known once the host's seat command, always the
            // first message, has been read: open the mesh right after it.
            Bridge.Control base = bridge.control;
            final boolean[] opened = { false };
            bridge.control = packet -> {
                base.receive(packet);
                if (!opened[0]) {
                    opened[0] = true;
                    try {
                        openMesh();
                    } catch (IOException e) {
                        report.write("event=mesh_open_failed");
                        listener.status(e.getMessage());
                    }
                }
            };
        }
    }

    private void prepare(byte[] attempt) {
        spawn("load game", () -> loadGame(attempt));
    }

    private void commit(byte[] attempt) {
        report.write("commit=" + Wire.hex(attempt));
        try {
            if (maxPlayers == 2 && bridge != null) {
                bridge.commitGame();
            }
            GameLink link = gameLink;
            if (link == null) {
                throw new IOException(Msg.s("Le jeu s'est fermé avant le départ.", "The game closed before the start."));
            }
            link.go();
        } catch (IOException e) {
            if (!disposed) {
                listener.failed(e.getMessage());
                close();
            }
        }
    }

    private void attachLobby() throws IOException {
        Bridge b = bridge;
        b.diagnostic = report::write;
        report.write("role=" + (host ? "host" : "guest") + " build=" + Wire.hex(build)
            + " tls=connected game_transport=udp-authenticated path=" + joinPath);
        lobby = new Lobby(host, profile, (seat, payload) -> b.sendControl(payload), this::prepare, this::commit, listener::changed);
        b.control = lobby::receive;
        b.ping = value -> {
            pingMs = value;
            listener.changed();
        };
        watch();
        lobby.announce();
        listener.changed();
        startDiagnostics();
    }

    private synchronized void startDiagnostics() {
        if (disposed || diagnosticTimer != null) {
            return;
        }
        diagnosticTimer = Executors.newSingleThreadScheduledExecutor();
        diagnosticTimer.scheduleWithFixedDelay(() -> {
            Lobby l = lobby;
            Bridge b = bridge;
            if (l == null) {
                return;
            }
            String line = "phase=" + l.phase() + " occupied=" + l.occupied() + "/" + maxPlayers + " disk_match=" + l.discMatches();
            if (b != null && maxPlayers == 2) {
                line += " ping_ms=" + pingMs + " udp_ready=" + b.udpReady() + " local_packets=" + b.gamePackets()
                    + " peer_packets=" + b.peerPackets() + " " + b.transportStatus();
            }
            report.write(line);
        }, 0, 5, TimeUnit.SECONDS);
    }

    // Maps this seat's own mesh port and announces it through the salon. Every
    // seat has to be dialable in a mesh, a guest's as much as the host's.
    private void openMesh() throws IOException {
        Lobby l = lobby;
        if (l == null) {
            throw new IOException("lobby not ready");
        }
        Gateway.Route meshRoute = route != null ? route : Gateway.Route.detect(context);
        DatagramChannel socket = DatagramChannel.open();
        try {
            socket.bind(new InetSocketAddress(meshRoute.local, 0));
            int port = ((InetSocketAddress) socket.getLocalAddress()).getPort();
            InetSocketAddress announced;
            InetAddress test = debuggable() ? testPublicAddress : null;
            if (test != null) {
                announced = new InetSocketAddress(test, port);
            } else {
                try {
                    meshMapping = new Gateway(context, meshRoute, port, port, true);
                    meshMapping.open();
                    announced = new InetSocketAddress(meshMapping.address, meshMapping.port());
                } catch (IOException e) {
                    meshMapping = null;
                    announced = new InetSocketAddress(meshRoute.local, port); // same Wi-Fi only
                    report.write("mesh_mapping=failed lan_only=true");
                }
            }
            checkDisposed();
            MeshRelay relay = new MeshRelay(l.localSeat(), invite.token, socket);
            relay.diagnostic = report::write;
            meshRelay = relay;
            l.endpointLearned = (seat, endpoint) -> {
                if (seat == l.localSeat() || relay.hasPeer(seat)) {
                    return;
                }
                try {
                    relay.addPeer(seat, endpoint);
                } catch (IOException ignored) {
                }
            };
            for (int seat = 0; seat < Lobby.MAX_SEATS; seat++) {
                InetSocketAddress known = l.peerEndpoint(seat);
                if (known != null && seat != l.localSeat() && !relay.hasPeer(seat)) {
                    relay.addPeer(seat, known);
                }
            }
            spawn("mesh", relay::run);
            if (meshMapping != null) {
                spawn("mesh maintain", this::maintainMesh);
            }
            l.announceEndpoint(announced);
        } catch (IOException | RuntimeException e) {
            Wire.closeQuietly(socket);
            if (meshMapping != null) {
                meshMapping.close();
                meshMapping = null;
            }
            throw e;
        }
    }

    private void maintainMesh() {
        try {
            while (!disposed) {
                Thread.sleep(Math.max(5, meshMapping.lifetime / 2) * 1000L);
                synchronized (this) {
                    if (disposed) {
                        return;
                    }
                    meshMapping.renew();
                }
            }
        } catch (InterruptedException ignored) {
        } catch (IOException e) {
            if (!disposed) {
                report.write("event=mesh_mapping_lost");
            }
        }
    }

    private void watch() {
        Bridge b = bridge;
        spawn("watch", () -> {
            String reason = Msg.s("Votre ami s'est déconnecté. Recréez une connexion.", "Your friend disconnected. Connect again.");
            try {
                b.run();
            } catch (IOException e) {
                report.write("transport_error=" + e.getClass().getSimpleName());
                if (e.getMessage() != null && !(e instanceof Bridge.ControlChannelException)) {
                    reason = e.getMessage();
                }
            } finally {
                if (!disposed) {
                    listener.failed(reason);
                    close();
                }
            }
        });
    }

    boolean fastChannelReady() {
        if (maxPlayers > 2) {
            MeshRelay relay = meshRelay;
            return relay != null && relay.allPeersReady(5000);
        }
        Bridge b = bridge;
        return b != null && b.udpReady();
    }

    void launch() throws IOException {
        Lobby l = lobby;
        if (l == null || disposed) {
            throw new IOException(Msg.s("Attendez que votre ami soit connecté.", "Wait for your friend to connect."));
        }
        if (!fastChannelReady()) {
            throw new IOException(Msg.s("Le canal rapide du jeu se prépare encore. Attendez deux secondes puis réessayez.",
                "The game's fast channel is still being prepared. Wait two seconds, then try again."));
        }
        l.start();
    }

    // Same flags the PC salon gives partyboard.exe: lockstep for the whole
    // session, three frames of input delay, the local pad as player one.
    static String[] onlineArguments(List<String> transport) {
        List<String> args = new ArrayList<>(transport);
        args.add("--netplay-loopback");
        args.add("--netplay-full");
        args.add("--netplay-pad");
        args.add("1");
        args.add("--netplay-delay");
        args.add("3");
        return args.toArray(new String[0]);
    }

    private static int reserveLoopbackPort() throws IOException {
        try (DatagramSocket reserve = new DatagramSocket(new InetSocketAddress(Wire.LOOPBACK, 0))) {
            return reserve.getLocalPort();
        }
    }

    private void loadGame(byte[] attempt) {
        GameLink link = null;
        try {
            report.write("loading=" + Wire.hex(attempt));
            if (disc == null || !Wire.equal(disc.hash, profile.discHash)) {
                throw new IOException(Msg.s("Le disque n'est pas vérifié.", "The disc is not verified."));
            }
            checkDisposed();
            link = new GameLink();
            gameLink = link;
            List<String> transport = new ArrayList<>();
            Lobby l = lobby;
            if (maxPlayers > 2) {
                MeshRelay relay = meshRelay;
                if (relay == null) {
                    throw new IOException(Msg.s("La mise en réseau directe n'est pas prête.", "The direct network is not ready."));
                }
                transport.add("--netplay-host");
                transport.add(String.valueOf(reserveLoopbackPort()));
                transport.add("--netplay-players");
                transport.add(String.valueOf(maxPlayers));
                transport.add("--netplay-seat");
                transport.add(String.valueOf(l.localSeat()));
                for (int seat = 0; seat < maxPlayers; seat++) {
                    if (seat != l.localSeat() && l.seatInfo(seat) != null) {
                        transport.add("--netplay-peer");
                        transport.add(seat + ":127.0.0.1:" + relay.loopbackPort(seat));
                    }
                }
            } else if (host) {
                transport.add("--netplay-host");
                transport.add(String.valueOf(hostGamePort));
            } else {
                transport.add("--netplay-join");
                transport.add("127.0.0.1:" + bridge.localPort());
            }
            report.write("netplay_mode=lockstep input_delay_frames=3");
            launcher.launch(onlineArguments(transport), link.barrier(), disc.path);
            link.waitReady(() -> disposed, launcher::gameAlive, 120_000);
            report.write("native_ready=" + Wire.hex(attempt));
            l.loaded(attempt);
            link.waitExit();
            report.write("native_exit");
            // Say goodbye while the control channel is still up.
            l.localGameExited();
            listener.failed(Msg.s("La partie est terminée. Créez un nouveau salon pour rejouer.",
                "The game is over. Create a new lobby to play again."));
            close();
        } catch (IOException | RuntimeException e) {
            if (!disposed) {
                listener.failed(e instanceof IOException && e.getMessage() != null ? e.getMessage()
                    : Msg.s("Le lancement a échoué. Quittez le salon puis réessayez.", "The launch failed. Leave the lobby and try again."));
                close();
            }
        } finally {
            if (link != null) {
                link.close();
            }
        }
    }

    // A readable summary of where the salon stands, for the window.
    String describePath() {
        return joinPath.toUpperCase(Locale.ROOT);
    }

    static boolean isIpv4(InetAddress a) {
        return a instanceof Inet4Address;
    }

    @Override
    public void close() {
        synchronized (this) {
            if (!disposed) {
                report.write("session_closed");
            }
            disposed = true;
            if (diagnosticTimer != null) {
                diagnosticTimer.shutdownNow();
            }
            Wire.closeQuietly(server);
            GameLink link = gameLink;
            if (link != null) {
                link.abort();
            }
            if (lobby != null) {
                lobby.close();
            }
            if (bridge != null) {
                bridge.close();
            }
            if (guestLinks != null) {
                for (Bridge g : guestLinks) {
                    if (g != null) {
                        g.close();
                    }
                }
            }
            Wire.closeQuietly(internetGame);
            if (meshRelay != null) {
                meshRelay.close();
            }
        }
        // Router cleanup talks to the network; never on the caller's thread.
        Gateway a = mappingUdp, b = mapping, c = meshMapping;
        mappingUdp = mapping = meshMapping = null;
        if (a != null || b != null || c != null) {
            spawn("unmap", () -> {
                if (a != null) {
                    a.close();
                }
                if (b != null) {
                    b.close();
                }
                if (c != null) {
                    c.close();
                }
            });
        }
    }
}
