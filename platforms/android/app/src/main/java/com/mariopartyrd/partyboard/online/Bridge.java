package com.mariopartyrd.partyboard.online;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.DatagramChannel;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

import javax.crypto.Mac;
import javax.net.ssl.SSLSocket;

// tools/online/Connection.cs (Bridge). TLS carries the lobby's control
// messages on two channels (one read, one write, so a slow write never blocks
// a read); the game's inputs go over authenticated UDP between the two phones,
// and over loopback UDP between this process and the local game process.
final class Bridge implements AutoCloseable {
    interface Control {
        void receive(byte[] packet) throws IOException;
    }

    interface Log {
        void write(String line);
    }

    // A failure of the salon's TLS link, as opposed to the game's own UDP.
    static final class ControlChannelException extends IOException {
        ControlChannelException(Throwable cause) {
            super(Msg.s("La connexion du salon a été interrompue.", "The lobby connection was interrupted."), cause);
        }
    }

    private static final byte[] POISON = new byte[0];

    private final SSLSocket readSsl;
    private final SSLSocket writeSsl;
    private final int localPlayer;
    private final int remotePlayer;
    private final DatagramChannel udp;
    private final DatagramChannel network;
    private volatile InetSocketAddress game;
    private volatile InetSocketAddress networkPeer;
    private final boolean learnNetworkPeer;
    private final Mac mac;
    private final Datagram.ReplayWindow replay = new Datagram.ReplayWindow();
    private final AtomicLong networkSequence = new AtomicLong();
    private final AtomicBoolean stopping = new AtomicBoolean();
    private volatile boolean closed;
    private final AtomicInteger committed = new AtomicInteger();
    private final AtomicInteger controlLost = new AtomicInteger();
    private final LinkedBlockingQueue<byte[]> toPeer = new LinkedBlockingQueue<>(4096);
    private volatile boolean addingCompleted;
    private final boolean controlOnly;
    private Thread heartbeat;

    volatile Control control;
    volatile java.util.function.IntConsumer ping;
    volatile Log diagnostic;

    private volatile long udpPingStamp;
    private final AtomicLong lastUdpPongStamp = new AtomicLong();
    private final AtomicLong lastUdpStamp = new AtomicLong();
    private final AtomicLong lastPumpStamp = new AtomicLong();
    private final AtomicLong deliveredPackets = new AtomicLong();
    private final AtomicLong udpDropped = new AtomicLong();
    private final AtomicLong gamePackets = new AtomicLong();
    private final AtomicLong peerPackets = new AtomicLong();
    private volatile String lastUdpError = "";

    // reader/writer are the guest's two channels seen from this side. For a
    // host, internetPeer is null: the guest's address is learned from its
    // first authenticated packet, which also follows a NAT that renumbers it.
    Bridge(SSLSocket reader, SSLSocket writer, int player, DatagramChannel internet, InetSocketAddress internetPeer,
        byte[] token, int hostGamePort, int peerPlayer, boolean controlOnly) throws IOException
    {
        readSsl = reader;
        writeSsl = writer;
        localPlayer = player;
        remotePlayer = peerPlayer < 0 ? player ^ 1 : peerPlayer;
        network = internet;
        networkPeer = internetPeer;
        learnNetworkPeer = internetPeer == null;
        mac = Datagram.mac(Datagram.key(token));
        this.controlOnly = controlOnly;
        udp = DatagramChannel.open();
        udp.bind(new InetSocketAddress(Wire.LOOPBACK, 0));
        if (hostGamePort != 0) {
            game = new InetSocketAddress(Wire.LOOPBACK, hostGamePort);
            udp.connect(game);
        }
    }

    static long now() {
        return System.nanoTime();
    }

    static long ageMs(long stamp) {
        return stamp == 0 ? -1 : (now() - stamp) / 1_000_000L;
    }

    boolean controlConnected() {
        return controlLost.get() == 0;
    }

    // Set only by the authenticated lobby commit, never by UDP input.
    void commitGame() throws IOException {
        if (closed || !controlConnected()) {
            throw new IOException(Msg.s("Le salon s'est fermé avant le départ.", "The lobby closed before the start."));
        }
        committed.set(1);
    }

    boolean udpReady() {
        long stamp = lastUdpPongStamp.get();
        return stamp != 0 && ageMs(stamp) < 5000;
    }

    int localPort() {
        try {
            return ((InetSocketAddress) udp.getLocalAddress()).getPort();
        } catch (IOException e) {
            return 0;
        }
    }

    long gamePackets() {
        return gamePackets.get();
    }

    long peerPackets() {
        return peerPackets.get();
    }

    String transportStatus() {
        return "control_connected=" + controlConnected() + " udp_age_ms=" + ageMs(lastUdpStamp.get())
            + " udp_pong_age_ms=" + ageMs(lastUdpPongStamp.get()) + " pump_age_ms=" + ageMs(lastPumpStamp.get())
            + " delivered_packets=" + deliveredPackets.get() + " udp_dropped=" + udpDropped.get()
            + " udp_error=" + lastUdpError;
    }

    private void log(String line) {
        Log d = diagnostic;
        if (d != null) {
            d.write(line);
        }
    }

    void sendControl(byte[] payload) throws IOException {
        if (payload.length < 1 || payload.length > 2048) {
            throw new IOException("lobby message too long");
        }
        byte[] frame = new byte[payload.length + 2];
        frame[0] = (byte) (payload.length >> 8);
        frame[1] = (byte) payload.length;
        System.arraycopy(payload, 0, frame, 2, payload.length);
        if (closed || !controlConnected() || addingCompleted) {
            throw new ControlChannelException(new IOException("control channel closed"));
        }
        try {
            if (!toPeer.offer(frame, 1000, TimeUnit.MILLISECONDS)) {
                throw new ControlChannelException(new IOException("control queue full"));
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new ControlChannelException(e);
        }
    }

    private void completeAdding() {
        addingCompleted = true;
        toPeer.clear();
        toPeer.offer(POISON);
    }

    private void recoverable(IOException e, String operation) throws IOException {
        if (e instanceof ClosedChannelException) {
            throw e;
        }
        // UDP ICMP errors and send pressure are packet loss: the native input
        // history repairs it, and neither closes the lobby.
        udpDropped.incrementAndGet();
        String kind = e.getClass().getSimpleName();
        if (!kind.equals(lastUdpError)) {
            lastUdpError = kind;
            log("transport_event=" + operation + " error=" + kind);
        }
    }

    private void sendGame(byte[] payload) throws IOException {
        InetSocketAddress target = networkPeer;
        if (target == null) {
            return;
        }
        byte[] packet = Datagram.seal(mac, localPlayer, networkSequence.incrementAndGet(), payload);
        try {
            network.send(ByteBuffer.wrap(packet), target);
        } catch (IOException e) {
            recoverable(e, "udp_send");
        }
    }

    private boolean keepCommittedGame() {
        if (closed || committed.get() == 0) {
            return false;
        }
        if (controlLost.compareAndSet(0, 1)) {
            log("event=control_lost game_transport=udp_continues");
            if (heartbeat != null) {
                heartbeat.interrupt();
            }
            completeAdding();
            // Closing the TLS sockets wakes both workers without touching the
            // game's UDP path or the start barrier.
            Wire.closeQuietly(readSsl);
            Wire.closeQuietly(writeSsl);
        }
        return true;
    }

    private byte[] readControl(InputStream in, int size) throws IOException {
        try {
            return Wire.readFully(in, size);
        } catch (IOException e) {
            throw new ControlChannelException(e);
        }
    }

    private void receiveLoop() throws IOException {
        InputStream in;
        try {
            in = readSsl.getInputStream();
        } catch (IOException e) {
            throw new ControlChannelException(e);
        }
        while (!closed && controlConnected()) {
            byte[] size = readControl(in, 2);
            int n = ((size[0] & 0xff) << 8) | (size[1] & 0xff);
            if (n < 1 || n > 2048) {
                throw new IOException(Msg.s("Message réseau incompatible.", "Incompatible network message."));
            }
            byte[] data = readControl(in, n);
            if (n == 1 && data[0] == 0) {
                continue;
            }
            if (n == 9 && data[0] == 6) {
                data[0] = 7;
                sendControl(data);
                continue;
            }
            if (n == 9 && data[0] == 7) {
                continue; // TLS probes only keep the link alive; latency comes from UDP.
            }
            // 2..5 profile and launch, 8 end of session, 9 seat, 11 endpoint.
            int type = data[0];
            if ((type >= 2 && type <= 5) || type == 8 || type == 9 || type == 11) {
                Control c = control;
                if (c == null) {
                    throw new IOException("lobby unavailable");
                }
                c.receive(data);
                continue;
            }
            throw new IOException(Msg.s("Message réseau incompatible.", "Incompatible network message."));
        }
    }

    private void writeLoop() throws IOException {
        OutputStream out;
        try {
            out = writeSsl.getOutputStream();
        } catch (IOException e) {
            throw new ControlChannelException(e);
        }
        ByteArrayOutputStream batch = new ByteArrayOutputStream(8192);
        while (!closed) {
            byte[] first;
            try {
                first = toPeer.take();
            } catch (InterruptedException e) {
                break;
            }
            if (first == POISON) {
                break;
            }
            batch.reset();
            batch.write(first, 0, first.length);
            int count = 1;
            byte[] next;
            while (count < 64 && batch.size() < 8192 && (next = toPeer.poll()) != null) {
                if (next == POISON) {
                    toPeer.offer(POISON);
                    break;
                }
                batch.write(next, 0, next.length);
                count++;
            }
            try {
                batch.writeTo(out);
                out.flush();
            } catch (IOException e) {
                throw new ControlChannelException(e);
            }
        }
    }

    private void heartbeatLoop() {
        try {
            Thread.sleep(250);
            while (!closed && controlConnected()) {
                byte[] probe = ByteBuffer.allocate(9).order(ByteOrder.LITTLE_ENDIAN).put((byte) 6).putLong(now()).array();
                try {
                    sendControl(probe);
                } catch (IOException e) {
                    if (controlOnly || !keepCommittedGame()) {
                        close();
                    }
                    return;
                }
                Thread.sleep(2000);
            }
        } catch (InterruptedException ignored) {
        }
    }

    private void pumpLoop() throws IOException {
        // One owner for both UDP sockets, heartbeat sends included.
        try (Selector selector = Selector.open()) {
            udp.configureBlocking(false);
            network.configureBlocking(false);
            udp.register(selector, SelectionKey.OP_READ);
            network.register(selector, SelectionKey.OP_READ);
            ByteBuffer buffer = ByteBuffer.allocate(2048);
            long nextHeartbeat = 0;
            long[] sequence = new long[1];
            while (!closed) {
                long stamp = now();
                lastPumpStamp.set(stamp);
                if (stamp - nextHeartbeat >= 0) {
                    udpPingStamp = stamp;
                    sendGame(Datagram.heartbeat(localPlayer, stamp));
                    nextHeartbeat = stamp + 1_000_000_000L;
                }
                long wait = Math.max(1, Math.min(250, (nextHeartbeat - now()) / 1_000_000L));
                selector.select(wait);
                selector.selectedKeys().clear();
                // Local game -> peer.
                for (int i = 0; i < 64 && !closed; i++) {
                    buffer.clear();
                    SocketAddress from;
                    try {
                        from = udp.receive(buffer);
                    } catch (IOException e) {
                        recoverable(e, "local_receive");
                        break;
                    }
                    if (from == null) {
                        break;
                    }
                    InetSocketAddress sender = (InetSocketAddress) from;
                    byte[] data = new byte[buffer.position()];
                    buffer.flip();
                    buffer.get(data);
                    if (!sender.getAddress().isLoopbackAddress() || !Datagram.isGamePacket(data, data.length, localPlayer)) {
                        continue;
                    }
                    if (game == null) {
                        game = sender;
                        udp.connect(game);
                    } else if (!game.equals(sender)) {
                        continue;
                    }
                    gamePackets.incrementAndGet();
                    sendGame(data);
                }
                // Peer -> local game.
                for (int i = 0; i < 64 && !closed; i++) {
                    buffer.clear();
                    SocketAddress from;
                    try {
                        from = network.receive(buffer);
                    } catch (IOException e) {
                        recoverable(e, "udp_receive");
                        break;
                    }
                    if (from == null) {
                        break;
                    }
                    int length = buffer.position();
                    byte[] payload = Datagram.open(mac, remotePlayer, buffer.array(), length, sequence);
                    if (payload == null) {
                        continue;
                    }
                    if (!learnNetworkPeer && !from.equals(networkPeer)) {
                        continue;
                    }
                    boolean heartbeat = Datagram.isHeartbeat(payload, remotePlayer);
                    if (!heartbeat && !Datagram.isGamePacket(payload, payload.length, remotePlayer)) {
                        continue;
                    }
                    boolean newest = replay.isNewest(remotePlayer, sequence[0]);
                    if (!replay.accept(remotePlayer, sequence[0])) {
                        continue;
                    }
                    if (learnNetworkPeer && newest) {
                        networkPeer = (InetSocketAddress) from;
                    }
                    lastUdpStamp.set(now());
                    if (heartbeat) {
                        if (payload[6] == 1) {
                            payload[5] = (byte) localPlayer;
                            payload[6] = 2;
                            sendGame(payload);
                        } else if (Datagram.heartbeatStamp(payload) == udpPingStamp) {
                            long receivedAt = now();
                            lastUdpPongStamp.set(receivedAt);
                            java.util.function.IntConsumer p = ping;
                            if (p != null) {
                                p.accept((int) Math.min(15000, (receivedAt - udpPingStamp) / 1_000_000L));
                            }
                        }
                        continue;
                    }
                    peerPackets.incrementAndGet();
                    if (game != null) {
                        try {
                            udp.write(ByteBuffer.wrap(payload));
                            deliveredPackets.incrementAndGet();
                        } catch (IOException e) {
                            recoverable(e, "local_send");
                        }
                    }
                }
            }
        }
    }

    private interface Work {
        void run() throws IOException;
    }

    private Thread worker(String name, Work work, AtomicReference<IOException> failure, Object done) {
        Thread t = new Thread(() -> {
            try {
                work.run();
            } catch (IOException | RuntimeException e) {
                if (!closed) {
                    log("worker=" + name + " error=" + e.getClass().getSimpleName());
                }
                if (e instanceof ControlChannelException && keepCommittedGame()) {
                    return;
                }
                failure.compareAndSet(null, e instanceof IOException ? (IOException) e : new IOException(e));
                close();
            } finally {
                synchronized (done) {
                    done.notifyAll();
                }
            }
        }, "PartyBoard " + name);
        t.setDaemon(true);
        return t;
    }

    // Blocks until the link ends; throws the reason when it failed.
    void run() throws IOException {
        AtomicReference<IOException> failure = new AtomicReference<>();
        Object done = new Object();
        Thread pump = controlOnly ? null : worker("udp_pump", this::pumpLoop, failure, done);
        Thread read = worker("tls_read", this::receiveLoop, failure, done);
        Thread write = worker("tls_write", this::writeLoop, failure, done);
        heartbeat = new Thread(this::heartbeatLoop, "PartyBoard heartbeat");
        heartbeat.setDaemon(true);
        if (pump != null) {
            pump.start();
        }
        read.start();
        write.start();
        heartbeat.start();
        try {
            synchronized (done) {
                while ((pump == null || pump.isAlive()) && read.isAlive() && write.isAlive()) {
                    done.wait(500);
                }
            }
            // A committed game keeps its UDP path after the control link drops.
            if (!closed && !controlConnected() && pump != null) {
                pump.join();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            close();
        }
        IOException e = failure.get();
        if (e != null) {
            throw e;
        }
    }

    @Override
    public void close() {
        if (!stopping.compareAndSet(false, true)) {
            return;
        }
        closed = true;
        if (heartbeat != null) {
            heartbeat.interrupt();
        }
        completeAdding();
        Wire.closeQuietly(readSsl);
        Wire.closeQuietly(writeSsl);
        Wire.closeQuietly(udp);
        Wire.closeQuietly(network);
    }
}
