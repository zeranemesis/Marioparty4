package com.mariopartyrd.partyboard.online;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.DatagramChannel;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

import javax.crypto.Mac;

// tools/online/Connection.cs (MeshRelay). Above two players every seat talks to
// every other directly. The native engine identifies a peer by the UDP address
// its packets come from, so each remote seat gets its own loopback port here,
// while one shared Internet socket demultiplexes by the authenticated seat byte.
final class MeshRelay implements AutoCloseable {
    private static final class Leg {
        final DatagramChannel loopback;
        volatile InetSocketAddress networkPeer;
        volatile InetSocketAddress game;
        final boolean learnPeer;
        volatile long lastPongStamp;

        Leg(InetSocketAddress peer) throws IOException {
            loopback = DatagramChannel.open();
            loopback.bind(new InetSocketAddress(Wire.LOOPBACK, 0));
            networkPeer = peer;
            learnPeer = peer == null;
        }

        int loopbackPort() {
            try {
                return ((InetSocketAddress) loopback.getLocalAddress()).getPort();
            } catch (IOException e) {
                return 0;
            }
        }
    }

    private final int localPlayer;
    private final Mac mac;
    private final DatagramChannel network;
    private final Leg[] legs = new Leg[Lobby.MAX_SEATS];
    private final Datagram.ReplayWindow replay = new Datagram.ReplayWindow();
    private final AtomicLong networkSequence = new AtomicLong();
    private final AtomicLong udpDropped = new AtomicLong();
    private final AtomicBoolean stopping = new AtomicBoolean();
    private volatile boolean closed;
    private volatile Selector selector;
    volatile Bridge.Log diagnostic;

    MeshRelay(int localPlayer, byte[] token, DatagramChannel internetSocket) {
        this.localPlayer = localPlayer;
        mac = Datagram.mac(Datagram.key(token));
        network = internetSocket;
    }

    synchronized int addPeer(int seat, InetSocketAddress peerEndpoint) throws IOException {
        if (seat < 0 || seat >= Lobby.MAX_SEATS || seat == localPlayer || legs[seat] != null) {
            throw new IOException("invalid mesh peer");
        }
        Leg leg = new Leg(peerEndpoint);
        legs[seat] = leg;
        Selector s = selector;
        if (s != null) {
            s.wakeup();
        }
        return leg.loopbackPort();
    }

    synchronized boolean hasPeer(int seat) {
        return seat >= 0 && seat < Lobby.MAX_SEATS && legs[seat] != null;
    }

    synchronized int loopbackPort(int seat) throws IOException {
        if (seat < 0 || seat >= Lobby.MAX_SEATS || legs[seat] == null) {
            throw new IOException(Msg.s("Ce siège n'a pas de pair de maillage.", "This seat has no mesh peer."));
        }
        return legs[seat].loopbackPort();
    }

    synchronized boolean allPeersReady(long maxAgeMs) {
        for (Leg leg : legs) {
            if (leg == null) {
                continue;
            }
            if (leg.lastPongStamp == 0 || Bridge.ageMs(leg.lastPongStamp) > maxAgeMs) {
                return false;
            }
        }
        return true;
    }

    private void log(String line) {
        Bridge.Log d = diagnostic;
        if (d != null) {
            d.write(line);
        }
    }

    private void recoverable(IOException e, String operation) throws IOException {
        if (e instanceof ClosedChannelException) {
            throw e;
        }
        if (udpDropped.incrementAndGet() == 1) {
            log("mesh_event=" + operation + " error=" + e.getClass().getSimpleName());
        }
    }

    private void sendToLeg(Leg leg, byte[] payload) throws IOException {
        InetSocketAddress target = leg.networkPeer;
        if (target == null) {
            return;
        }
        byte[] packet = Datagram.seal(mac, localPlayer, networkSequence.incrementAndGet(), payload);
        try {
            network.send(ByteBuffer.wrap(packet), target);
        } catch (IOException e) {
            recoverable(e, "mesh_send");
        }
    }

    void run() {
        try (Selector s = Selector.open()) {
            selector = s;
            network.configureBlocking(false);
            network.register(s, SelectionKey.OP_READ);
            boolean[] registered = new boolean[Lobby.MAX_SEATS];
            ByteBuffer buffer = ByteBuffer.allocate(2048);
            long[] sequence = new long[1];
            long nextHeartbeat = 0;
            while (!closed) {
                Leg[] current;
                synchronized (this) {
                    current = legs.clone();
                }
                for (int seat = 0; seat < current.length; seat++) {
                    if (current[seat] != null && !registered[seat]) {
                        current[seat].loopback.configureBlocking(false);
                        current[seat].loopback.register(s, SelectionKey.OP_READ);
                        registered[seat] = true;
                    }
                }
                long stamp = Bridge.now();
                if (stamp - nextHeartbeat >= 0) {
                    byte[] beat = Datagram.heartbeat(localPlayer, stamp);
                    for (Leg leg : current) {
                        if (leg != null) {
                            sendToLeg(leg, beat);
                        }
                    }
                    nextHeartbeat = stamp + 1_000_000_000L;
                }
                s.select(Math.max(1, Math.min(250, (nextHeartbeat - Bridge.now()) / 1_000_000L)));
                s.selectedKeys().clear();
                // Local game -> that seat's remote endpoint.
                for (Leg leg : current) {
                    if (leg == null) {
                        continue;
                    }
                    for (int i = 0; i < 64 && !closed; i++) {
                        buffer.clear();
                        SocketAddress from;
                        try {
                            from = leg.loopback.receive(buffer);
                        } catch (IOException e) {
                            recoverable(e, "mesh_local_receive");
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
                        if (leg.game == null) {
                            leg.game = sender;
                        } else if (!leg.game.equals(sender)) {
                            continue;
                        }
                        sendToLeg(leg, data);
                    }
                }
                // Any remote seat -> its own loopback leg. The seat byte is only
                // trusted once the HMAC over it has been checked.
                for (int i = 0; i < 256 && !closed; i++) {
                    buffer.clear();
                    SocketAddress from;
                    try {
                        from = network.receive(buffer);
                    } catch (IOException e) {
                        recoverable(e, "mesh_network_receive");
                        break;
                    }
                    if (from == null) {
                        break;
                    }
                    int length = buffer.position();
                    if (length != Datagram.SIZE) {
                        continue;
                    }
                    int seat = buffer.array()[5] & 0xff;
                    if (seat >= Lobby.MAX_SEATS || current[seat] == null) {
                        continue;
                    }
                    Leg leg = current[seat];
                    byte[] payload = Datagram.open(mac, seat, buffer.array(), length, sequence);
                    if (payload == null || (!leg.learnPeer && !from.equals(leg.networkPeer))) {
                        continue;
                    }
                    boolean heartbeat = Datagram.isHeartbeat(payload, seat);
                    if (!heartbeat && !Datagram.isGamePacket(payload, payload.length, seat)) {
                        continue;
                    }
                    boolean newest = replay.isNewest(seat, sequence[0]);
                    if (!replay.accept(seat, sequence[0])) {
                        continue;
                    }
                    if (leg.learnPeer && newest) {
                        leg.networkPeer = (InetSocketAddress) from;
                    }
                    if (heartbeat) {
                        if (payload[6] == 1) {
                            payload[5] = (byte) localPlayer;
                            payload[6] = 2;
                            sendToLeg(leg, payload);
                        } else {
                            leg.lastPongStamp = Bridge.now();
                        }
                        continue;
                    }
                    if (leg.game == null) {
                        continue;
                    }
                    try {
                        leg.loopback.send(ByteBuffer.wrap(payload), leg.game);
                    } catch (IOException e) {
                        recoverable(e, "mesh_local_send");
                    }
                }
            }
        } catch (IOException | RuntimeException e) {
            if (!closed) {
                log("mesh_worker_error=" + e.getClass().getSimpleName());
            }
        } finally {
            close();
        }
    }

    @Override
    public void close() {
        if (!stopping.compareAndSet(false, true)) {
            return;
        }
        closed = true;
        Selector s = selector;
        if (s != null) {
            s.wakeup();
        }
        Wire.closeQuietly(network);
        synchronized (this) {
            for (Leg leg : legs) {
                if (leg != null) {
                    Wire.closeQuietly(leg.loopback);
                }
            }
        }
    }
}
