package com.mariopartyrd.partyboard.online;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

// tools/online/Lobby.cs: the salon's state machine. Role checks live here, not
// only in a disabled button. The host is seat 0 and the only one holding the
// whole roster; every guest compares itself to seat 0 and to nobody else.
final class Lobby {
    static final int MAX_SEATS = 4;
    static final int BROADCAST = -1;

    enum Phase { WAITING, PREPARING, RUNNING, CLOSED }

    enum Ending { NONE, LOCAL_GAME_CLOSED, REMOTE_GAME_CLOSED }

    interface Sender {
        void send(int seat, byte[] payload) throws IOException;
    }

    interface AttemptCallback {
        void run(byte[] attempt);
    }

    interface EndpointListener {
        void learned(int seat, InetSocketAddress endpoint);
    }

    // ---- ModSet (tools/online/Mods.cs) -------------------------------------
    // Android has no mod manager, so a phone always announces an empty list.
    // A peer's list is still decoded and compared, which is what refuses a
    // session against a modded host instead of letting it desync.
    static final class ModEntry {
        final int id;
        final String name;
        final byte[] fingerprint;

        ModEntry(int id, String name, byte[] fingerprint) throws IOException {
            if (id <= 0 || fingerprint == null || fingerprint.length != 8) {
                throw new IOException("invalid mod");
            }
            this.id = id;
            this.name = name == null ? "" : name;
            this.fingerprint = fingerprint.clone();
        }

        String describe() {
            return name.isEmpty() ? "#" + id : name + " (#" + id + ")";
        }
    }

    static final class ModSet {
        static final int NAME_BYTES = 48;
        static final int MAX_MODS = 24;
        static final ModSet EMPTY = new ModSet(new ArrayList<>());
        final List<ModEntry> entries;

        ModSet(List<ModEntry> entries) {
            this.entries = entries;
        }

        boolean none() {
            return entries.isEmpty();
        }

        boolean same(ModSet other) {
            if (other == null || other.entries.size() != entries.size()) {
                return false;
            }
            for (int i = 0; i < entries.size(); i++) {
                if (entries.get(i).id != other.entries.get(i).id
                    || !Wire.equal(entries.get(i).fingerprint, other.entries.get(i).fingerprint))
                {
                    return false;
                }
            }
            return true;
        }

        ModEntry find(int id) {
            for (ModEntry e : entries) {
                if (e.id == id) {
                    return e;
                }
            }
            return null;
        }

        String differenceFrom(ModSet required) {
            if (required == null) {
                return Msg.s("Les mods de l'autre joueur ne sont pas encore connus.", "The other player's mods are not known yet.");
            }
            List<String> missing = new ArrayList<>(), extra = new ArrayList<>(), differing = new ArrayList<>();
            for (ModEntry r : required.entries) {
                ModEntry m = find(r.id);
                if (m == null) {
                    missing.add(r.describe());
                } else if (!Wire.equal(m.fingerprint, r.fingerprint)) {
                    differing.add(r.describe());
                }
            }
            for (ModEntry e : entries) {
                if (required.find(e.id) == null) {
                    extra.add(e.describe());
                }
            }
            List<String> parts = new ArrayList<>();
            if (!missing.isEmpty()) {
                parts.add(Msg.s("à installer : ", "to install: ") + String.join(", ", missing));
            }
            if (!extra.isEmpty()) {
                parts.add(Msg.s("à désactiver : ", "to disable: ") + String.join(", ", extra));
            }
            if (!differing.isEmpty()) {
                parts.add(Msg.s("version différente : ", "different version: ") + String.join(", ", differing));
            }
            if (parts.isEmpty() && !same(required)) {
                parts.add(Msg.s("mêmes mods mais dans un autre ordre", "same mods in a different order"));
            }
            return String.join(" • ", parts);
        }

        void write(ByteArrayOutputStream w) throws IOException {
            w.write(entries.size());
            for (ModEntry e : entries) {
                w.write(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(e.id).array());
                w.write(e.fingerprint);
                byte[] name = e.name.getBytes(StandardCharsets.UTF_8);
                if (name.length > NAME_BYTES + 4) {
                    throw new IOException("mod name too long");
                }
                w.write(name.length);
                w.write(name);
            }
        }

        static ModSet read(ByteBuffer r) throws IOException {
            int count = r.get() & 0xff;
            if (count > MAX_MODS) {
                throw new IOException("too many mods");
            }
            List<ModEntry> entries = new ArrayList<>();
            for (int i = 0; i < count; i++) {
                int id = r.getInt();
                byte[] fingerprint = new byte[8];
                r.get(fingerprint);
                int n = r.get() & 0xff;
                if (n > NAME_BYTES + 4) {
                    throw new IOException("mod name too long");
                }
                byte[] name = new byte[n];
                r.get(name);
                entries.add(new ModEntry(id, strictUtf8(name), fingerprint));
            }
            return new ModSet(entries);
        }
    }

    static String strictUtf8(byte[] bytes) throws IOException {
        try {
            CharBuffer c = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes));
            return c.toString();
        } catch (CharacterCodingException e) {
            throw new IOException("invalid UTF-8");
        }
    }

    // ---- PlayerInfo ------------------------------------------------------
    static final class PlayerInfo {
        final String name;
        final byte[] discHash;
        final long discLength;
        final ModSet mods;

        PlayerInfo(String name, byte[] hash, long length, ModSet mods) throws IOException {
            this.name = cleanName(name);
            if (hash != null && (hash.length != 32 || length <= 0)) {
                throw new IOException("invalid disc information");
            }
            discHash = hash == null ? null : hash.clone();
            discLength = hash == null ? 0 : length;
            this.mods = mods == null ? ModSet.EMPTY : mods;
        }

        static String cleanName(String name) throws IOException {
            name = name == null ? "" : name.trim();
            boolean bad = name.length() < 1 || name.length() > 24;
            for (int i = 0; !bad && i < name.length(); i++) {
                char c = name.charAt(i);
                bad = Character.isISOControl(c) || Character.isSurrogate(c);
            }
            if (bad) {
                throw new IOException(Msg.s("Choisissez un pseudo de 1 à 24 caractères, sans retour à la ligne.",
                    "Choose a nickname of 1 to 24 characters, on a single line."));
            }
            return name;
        }

        boolean sameDisc(PlayerInfo other) {
            return other != null && discHash != null && other.discHash != null && discLength == other.discLength
                && Wire.equal(discHash, other.discHash);
        }

        boolean sameMods(PlayerInfo other) {
            return other != null && mods.same(other.mods);
        }

        byte[] encode() throws IOException {
            ByteArrayOutputStream w = new ByteArrayOutputStream();
            byte[] n = name.getBytes(StandardCharsets.UTF_8);
            w.write(2);
            w.write(n.length);
            w.write(n);
            w.write(discHash != null ? 1 : 0);
            if (discHash != null) {
                w.write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(discLength).array());
                w.write(discHash);
            }
            mods.write(w);
            return w.toByteArray();
        }

        static PlayerInfo decode(byte[] data) throws IOException {
            try {
                ByteBuffer r = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
                if (r.get() != 2) {
                    throw new IOException();
                }
                int n = r.get() & 0xff;
                if (n < 1 || n > 96) {
                    throw new IOException();
                }
                byte[] nameBytes = new byte[n];
                r.get(nameBytes);
                String name = strictUtf8(nameBytes);
                int present = r.get() & 0xff;
                if (present > 1) {
                    throw new IOException();
                }
                long length = present == 1 ? r.getLong() : 0;
                byte[] hash = null;
                if (present == 1) {
                    hash = new byte[32];
                    r.get(hash);
                }
                // Decode insists on consuming the message exactly: a peer whose
                // announcement carries more than we understand is refused loudly,
                // never read short into a session that would desync.
                ModSet mods = ModSet.read(r);
                if (r.hasRemaining()) {
                    throw new IOException();
                }
                return new PlayerInfo(name, hash, length, mods);
            } catch (IOException | RuntimeException e) {
                throw new IOException(Msg.s("Informations du joueur incompatibles.", "Incompatible player information."));
            }
        }
    }

    // ---- The salon ---------------------------------------------------------
    private final Sender send;
    private final AttemptCallback prepare;
    private final AttemptCallback commit;
    private final Runnable changed;
    private final PlayerInfo[] seats = new PlayerInfo[MAX_SEATS];
    private final boolean[] ready = new boolean[MAX_SEATS];
    private final InetSocketAddress[] endpoints = new InetSocketAddress[MAX_SEATS];
    final boolean host;
    private int localSeat;
    private Phase phase = Phase.WAITING;
    private Ending ending = Ending.NONE;
    private byte[] attempt = new byte[16];
    volatile EndpointListener endpointLearned;

    Lobby(boolean host, PlayerInfo local, Sender sender, AttemptCallback loader, AttemptCallback starter, Runnable refresh) {
        this.host = host;
        send = sender;
        prepare = loader;
        commit = starter;
        changed = refresh;
        localSeat = host ? 0 : 1; // a guest keeps 1 until the host says otherwise
        seats[localSeat] = local;
    }

    synchronized int localSeat() {
        return localSeat;
    }

    synchronized Phase phase() {
        return phase;
    }

    synchronized Ending ending() {
        return ending;
    }

    synchronized PlayerInfo local() {
        return seats[localSeat];
    }

    synchronized PlayerInfo seatInfo(int index) {
        return index >= 0 && index < MAX_SEATS ? seats[index] : null;
    }

    synchronized int occupied() {
        int n = 0;
        for (PlayerInfo s : seats) {
            if (s != null) {
                n++;
            }
        }
        return n;
    }

    private boolean agreesWithHost(int seat) {
        PlayerInfo h = seats[0], other = seats[seat];
        return h != null && other != null && h.sameDisc(other) && h.sameMods(other);
    }

    synchronized boolean seatAgrees(int seat) {
        return seat >= 0 && seat < MAX_SEATS && agreesWithHost(seat);
    }

    private boolean everyoneAgrees() {
        if (seats[0] == null) {
            return false;
        }
        for (int i = 1; i < MAX_SEATS; i++) {
            if (seats[i] != null && !agreesWithHost(i)) {
                return false;
            }
        }
        return true;
    }

    synchronized boolean canStart() {
        return host && phase == Phase.WAITING && occupied() >= 2 && everyoneAgrees();
    }

    synchronized boolean discMatches() {
        if (seats[0] == null || occupied() < 2) {
            return false;
        }
        for (int i = 1; i < MAX_SEATS; i++) {
            if (seats[i] != null && !seats[0].sameDisc(seats[i])) {
                return false;
            }
        }
        return true;
    }

    synchronized boolean modsMatch() {
        if (seats[0] == null || occupied() < 2) {
            return false;
        }
        for (int i = 1; i < MAX_SEATS; i++) {
            if (seats[i] != null && !seats[0].sameMods(seats[i])) {
                return false;
            }
        }
        return true;
    }

    synchronized String modAdvice() {
        if (seats[0] == null) {
            return "";
        }
        if (!host) {
            return seats[localSeat].mods.same(seats[0].mods) ? "" : seats[localSeat].mods.differenceFrom(seats[0].mods);
        }
        List<String> parts = new ArrayList<>();
        for (int i = 1; i < MAX_SEATS; i++) {
            if (seats[i] == null || seats[i].mods.same(seats[0].mods)) {
                continue;
            }
            parts.add(seats[i].name + " : " + seats[i].mods.differenceFrom(seats[0].mods));
        }
        return String.join(" - ", parts);
    }

    private void fire() {
        if (changed != null) {
            changed.run();
        }
    }

    synchronized void announce() throws IOException {
        if (phase == Phase.WAITING) {
            send.send(BROADCAST, seats[localSeat].encode());
        }
    }

    static byte[] command(int type, byte[] id) {
        byte[] b = new byte[17];
        b[0] = (byte) type;
        System.arraycopy(id, 0, b, 1, 16);
        return b;
    }

    static byte[] seatCommand(int seat) {
        return new byte[] { 9, (byte) seat };
    }

    static byte[] endpointCommand(int subjectSeat, InetSocketAddress endpoint) throws IOException {
        byte[] address = endpoint.getAddress().getAddress();
        if (address.length != 4) {
            throw new IOException("IPv4 only");
        }
        byte[] packet = new byte[8];
        packet[0] = 11;
        packet[1] = (byte) subjectSeat;
        System.arraycopy(address, 0, packet, 2, 4);
        packet[6] = (byte) (endpoint.getPort() >> 8);
        packet[7] = (byte) endpoint.getPort();
        return packet;
    }

    private static InetSocketAddress decodeEndpoint(byte[] packet) {
        if (packet.length != 8 || packet[0] != 11) {
            return null;
        }
        int port = ((packet[6] & 0xff) << 8) | (packet[7] & 0xff);
        if (port <= 0) {
            return null;
        }
        try {
            return new InetSocketAddress(InetAddress.getByAddress(Arrays.copyOfRange(packet, 2, 6)), port);
        } catch (IOException e) {
            return null;
        }
    }

    synchronized InetSocketAddress peerEndpoint(int seat) {
        return seat >= 0 && seat < MAX_SEATS ? endpoints[seat] : null;
    }

    synchronized void announceEndpoint(InetSocketAddress endpoint) throws IOException {
        endpoints[localSeat] = endpoint;
        send.send(host ? BROADCAST : 0, endpointCommand(localSeat, endpoint));
    }

    // The seat is reserved before its profile is known: the guest announces
    // itself over the very link this seat number identifies.
    synchronized void admit(int guest) throws IOException {
        if (!host || guest < 1 || guest >= MAX_SEATS || phase != Phase.WAITING) {
            throw new IOException("admit refused");
        }
        send.send(guest, seatCommand(guest));
        send.send(guest, seats[0].encode());
        for (int seat = 0; seat < MAX_SEATS; seat++) {
            if (seat != guest && endpoints[seat] != null) {
                send.send(guest, endpointCommand(seat, endpoints[seat]));
            }
        }
        fire();
    }

    synchronized void leave(int guest) {
        if (guest < 1 || guest >= MAX_SEATS || guest == localSeat) {
            return;
        }
        seats[guest] = null;
        ready[guest] = false;
        endpoints[guest] = null;
        fire();
    }

    synchronized void start() throws IOException {
        if (!canStart()) {
            if (!host) {
                throw new IOException(Msg.s("Seul l'hôte peut lancer la partie.", "Only the host can start the game."));
            }
            if (occupied() < 2) {
                throw new IOException(Msg.s("Il faut au moins deux joueurs.", "At least two players are needed."));
            }
            if (!discMatches()) {
                throw new IOException(Msg.s("Tous les joueurs doivent avoir exactement le même fichier disque.",
                    "Every player needs exactly the same disc file."));
            }
            throw new IOException(Msg.s("Tous les joueurs doivent avoir exactement les mêmes mods actifs : ",
                "Every player needs exactly the same active mods: ") + modAdvice());
        }
        attempt = Wire.random(16);
        phase = Phase.PREPARING;
        Arrays.fill(ready, false);
        send.send(BROADCAST, command(3, attempt));
        prepare.run(attempt.clone());
        fire();
    }

    synchronized void loaded(byte[] id) throws IOException {
        if (phase != Phase.PREPARING || !Arrays.equals(id, attempt) || ready[localSeat]) {
            return;
        }
        ready[localSeat] = true;
        if (host) {
            tryCommit();
        } else {
            send.send(0, command(4, attempt));
        }
        fire();
    }

    private void tryCommit() throws IOException {
        if (!host) {
            return;
        }
        for (int i = 0; i < MAX_SEATS; i++) {
            if (seats[i] != null && !ready[i]) {
                return;
            }
        }
        send.send(BROADCAST, command(5, attempt));
        phase = Phase.RUNNING;
        commit.run(attempt.clone());
    }

    // Two-player wiring has one link: a host hears from seat 1, a guest from 0.
    void receive(byte[] packet) throws IOException {
        receive(host ? 1 : 0, packet);
    }

    // from is the seat the message arrived on, which the transport knows and
    // the message does not: nobody can claim to be someone else by writing it.
    synchronized void receive(int from, byte[] packet) throws IOException {
        if (phase == Phase.CLOSED) {
            throw new IOException("lobby closed");
        }
        if (from < 0 || from >= MAX_SEATS || from == localSeat) {
            throw new IOException("unknown seat");
        }
        if (packet.length > 0 && packet[0] == 2) {
            if (phase != Phase.WAITING) {
                throw new IOException("profile changed during launch");
            }
            seats[from] = PlayerInfo.decode(packet);
            fire();
            return;
        }
        if (packet.length == 2 && packet[0] == 9) {
            if (host || from != 0 || phase != Phase.WAITING) {
                throw new IOException("seat command refused");
            }
            int seat = packet[1] & 0xff;
            if (seat < 1 || seat >= MAX_SEATS) {
                throw new IOException("invalid seat");
            }
            if (seat != localSeat) {
                PlayerInfo mine = seats[localSeat];
                seats[localSeat] = null;
                localSeat = seat;
                seats[seat] = mine;
            }
            fire();
            return;
        }
        if (packet.length == 8 && packet[0] == 11) {
            InetSocketAddress endpoint = decodeEndpoint(packet);
            int subject = packet[1] & 0xff;
            if (endpoint == null || subject >= MAX_SEATS) {
                throw new IOException("invalid peer address");
            }
            if (host) {
                // Self-announcement only: this is the roster other guests trust.
                if (subject != from) {
                    throw new IOException("a player can only announce its own address");
                }
                endpoints[subject] = endpoint;
                for (int seat = 1; seat < MAX_SEATS; seat++) {
                    if (seats[seat] != null && seat != from) {
                        send.send(seat, packet);
                    }
                }
            } else {
                endpoints[subject] = endpoint;
            }
            EndpointListener listener = endpointLearned;
            if (listener != null) {
                listener.learned(subject, endpoint);
            }
            fire();
            return;
        }
        if (packet.length != 17) {
            throw new IOException("invalid lobby command");
        }
        byte[] id = Arrays.copyOfRange(packet, 1, 17);
        switch (packet[0]) {
            case 3:
                if (host || from != 0 || phase != Phase.WAITING || !agreesWithHost(localSeat) || Arrays.equals(id, new byte[16])) {
                    throw new IOException("launch request refused");
                }
                attempt = id;
                phase = Phase.PREPARING;
                Arrays.fill(ready, false);
                prepare.run(attempt.clone());
                break;
            case 4:
                if (!host || phase != Phase.PREPARING || !Arrays.equals(id, attempt) || ready[from]) {
                    throw new IOException("unexpected ready");
                }
                ready[from] = true;
                tryCommit();
                break;
            case 5:
                if (host || from != 0 || phase != Phase.PREPARING || !Arrays.equals(id, attempt) || !ready[localSeat]) {
                    throw new IOException("start refused");
                }
                phase = Phase.RUNNING;
                commit.run(attempt.clone());
                break;
            case 8:
                // A player closed their game; the host relays it to the others.
                if (phase != Phase.RUNNING && phase != Phase.PREPARING) {
                    throw new IOException("unexpected end");
                }
                if (!Arrays.equals(id, attempt)) {
                    throw new IOException("stale end");
                }
                if (host) {
                    for (int i = 1; i < MAX_SEATS; i++) {
                        if (seats[i] != null && i != from) {
                            try {
                                send.send(i, command(8, attempt));
                            } catch (IOException ignored) {
                            }
                        }
                    }
                }
                ending = Ending.REMOTE_GAME_CLOSED;
                phase = Phase.CLOSED;
                break;
            default:
                throw new IOException("unknown lobby command");
        }
        fire();
    }

    // Our own game has exited: say so before anything closes the sockets.
    synchronized void localGameExited() {
        if (phase == Phase.CLOSED) {
            return;
        }
        boolean announce = phase == Phase.RUNNING || phase == Phase.PREPARING;
        ending = Ending.LOCAL_GAME_CLOSED;
        phase = Phase.CLOSED;
        if (announce) {
            try {
                send.send(BROADCAST, command(8, attempt));
            } catch (IOException ignored) {
            }
        }
        fire();
    }

    synchronized void close() {
        phase = Phase.CLOSED;
        fire();
    }
}
