package com.mariopartyrd.partyboard.online;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.util.Log;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// Lobbies on the same Wi-Fi, found without passing the invitation around: a
// host announces its invitation on the local network every second while seats
// are free, and a lobby screen with no session lists what it hears, each with
// a Join button. Typing a 72-character code is what a headset makes hard (a
// Quest and a phone in the same room), and both sides are on the same network
// anyway.
//
// The announce is the invitation itself, so whoever is on the Wi-Fi can join
// the lobby while it has free seats -- what a nearby lobby is for. It stops as
// soon as the lobby is full or the game runs, and the host still sees who is
// in the lobby before starting.
final class Nearby {
    private static final String TAG = "PartyBoardOnline";
    static final int PORT = 47474;
    private static final String MAGIC = "PBNEAR1";
    private static final long FORGET_MS = 4000;

    // What the host announces, or null when it does not.
    interface Announce {
        String payload();
    }

    static final class Found {
        final String name;
        final int players;
        final int maxPlayers;
        final String invitation;
        long seenMs;

        Found(String name, int players, int maxPlayers, String invitation, long seenMs) {
            this.name = name;
            this.players = players;
            this.maxPlayers = maxPlayers;
            this.invitation = invitation;
            this.seenMs = seenMs;
        }

        boolean full() {
            return players >= maxPlayers;
        }
    }

    private final Context context;
    private final Announce announce;
    private final Runnable changed;
    private final Map<String, Found> found = new LinkedHashMap<>();
    private volatile boolean listening;
    private volatile boolean running = true;
    private final Thread thread;

    Nearby(Context context, Announce announce, Runnable changed) {
        this.context = context.getApplicationContext();
        this.announce = announce;
        this.changed = changed;
        thread = new Thread(this::run, "PartyBoard nearby");
        thread.setDaemon(true);
        thread.start();
    }

    static String payload(String name, int players, int maxPlayers, String invitation) {
        String clean = name == null ? "" : name.replace('|', ' ').replace('\n', ' ').trim();
        return MAGIC + "|" + clean + "|" + players + "|" + maxPlayers + "|" + invitation;
    }

    // While a lobby screen without a session is shown.
    void setListening(boolean on) {
        listening = on;
        if (!on) {
            boolean had;
            synchronized (found) {
                had = !found.isEmpty();
                found.clear();
            }
            if (had) changed.run();
        }
    }

    List<Found> found() {
        synchronized (found) {
            return Collections.unmodifiableList(new ArrayList<>(found.values()));
        }
    }

    void close() {
        running = false;
        thread.interrupt();
    }

    private void run() {
        DatagramSocket socket = null;
        WifiManager.MulticastLock lock = null;
        long lastAnnounce = 0;
        byte[] buffer = new byte[512];
        while (running) {
            String outgoing = announce.payload();
            boolean wanted = listening || outgoing != null;
            try {
                if (!wanted) {
                    if (socket != null) {
                        socket.close();
                        socket = null;
                    }
                    if (lock != null && lock.isHeld()) lock.release();
                    Thread.sleep(1000);
                    continue;
                }
                if (socket == null) {
                    socket = new DatagramSocket(null);
                    socket.setReuseAddress(true);
                    socket.setBroadcast(true);
                    socket.bind(new InetSocketAddress(PORT));
                    socket.setSoTimeout(250);
                }
                // Some phones drop broadcasts on Wi-Fi without it.
                if (listening && lock == null) {
                    WifiManager wifi = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
                    if (wifi != null) {
                        lock = wifi.createMulticastLock("PartyBoard nearby");
                        lock.setReferenceCounted(false);
                    }
                }
                if (lock != null) {
                    if (listening && !lock.isHeld()) lock.acquire();
                    if (!listening && lock.isHeld()) lock.release();
                }
                long now = System.currentTimeMillis();
                if (outgoing != null && now - lastAnnounce >= 1000) {
                    lastAnnounce = now;
                    send(socket, outgoing);
                }
                try {
                    DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                    socket.receive(packet);
                    if (listening) {
                        heard(new String(packet.getData(), 0, packet.getLength(), StandardCharsets.UTF_8), outgoing);
                    }
                } catch (SocketTimeoutException ignored) {
                }
                expire();
            } catch (InterruptedException e) {
                break;
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "Nearby lobbies: " + e.getMessage());
                if (socket != null) {
                    socket.close();
                    socket = null;
                }
                try {
                    Thread.sleep(2000);
                } catch (InterruptedException interrupted) {
                    break;
                }
            }
        }
        if (socket != null) socket.close();
        if (lock != null && lock.isHeld()) lock.release();
    }

    // Everywhere on this network: the global broadcast, which some routers
    // drop, and each interface's own.
    private static void send(DatagramSocket socket, String text) throws IOException {
        byte[] data = text.getBytes(StandardCharsets.UTF_8);
        List<InetAddress> targets = new ArrayList<>();
        targets.add(InetAddress.getByName("255.255.255.255"));
        Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
        while (interfaces != null && interfaces.hasMoreElements()) {
            NetworkInterface ni = interfaces.nextElement();
            if (!ni.isUp() || ni.isLoopback()) continue;
            for (InterfaceAddress address : ni.getInterfaceAddresses()) {
                InetAddress broadcast = address.getBroadcast();
                if (broadcast != null && !targets.contains(broadcast)) targets.add(broadcast);
            }
        }
        for (InetAddress target : targets) {
            try {
                socket.send(new DatagramPacket(data, data.length, target, PORT));
            } catch (IOException ignored) {
                // One interface refusing does not stop the others.
            }
        }
    }

    private void heard(String text, String own) {
        if (own != null && own.equals(text)) return; // our own announce
        String[] parts = text.split("\\|", -1);
        if (parts.length != 5 || !MAGIC.equals(parts[0])) return;
        int players, maxPlayers;
        try {
            players = Integer.parseInt(parts[2]);
            maxPlayers = Integer.parseInt(parts[3]);
        } catch (NumberFormatException e) {
            return;
        }
        String code = parts[4];
        try {
            Invitation.decode(code, false); // well formed, not expired
        } catch (IOException e) {
            return;
        }
        String name = parts[1].isEmpty() ? Msg.s("Joueur", "Player") : parts[1];
        if (name.length() > 24) name = name.substring(0, 24);
        boolean news;
        synchronized (found) {
            Found f = found.get(code);
            news = f == null || f.players != players || !f.name.equals(name);
            found.put(code, new Found(name, players, maxPlayers, code, System.currentTimeMillis()));
        }
        if (news) changed.run();
    }

    private void expire() {
        long now = System.currentTimeMillis();
        boolean removed = false;
        synchronized (found) {
            for (java.util.Iterator<Found> i = found.values().iterator(); i.hasNext(); ) {
                if (now - i.next().seenMs > FORGET_MS) {
                    i.remove();
                    removed = true;
                }
            }
        }
        if (removed) changed.run();
    }
}
