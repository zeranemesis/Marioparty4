package com.mariopartyrd.partyboard.online;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.function.BooleanSupplier;

// The Android side of GameStart's READY/GO events (tools/online/Lobby.cs).
// Windows uses named events; here the game connects back over loopback TCP
// with a one-time secret (src/port/portmain.cpp, online_wait_for_start):
//
//   game  -> lobby  "PBGAME1\n" + 32 hex secret + 'R'   (initialised, waiting)
//   lobby -> game   'G' to start, 'C' to give up
//
// The game keeps the socket open for the rest of its life, so the lobby learns
// the game has exited from the end of the stream, whatever way it ended.
final class GameLink implements AutoCloseable {
    private static final byte[] HELLO = "PBGAME1\n".getBytes(StandardCharsets.US_ASCII);

    private final ServerSocket server;
    private final String secret = Wire.hex(Wire.random(16));
    private volatile Socket game;
    private volatile boolean closed;

    GameLink() throws IOException {
        server = new ServerSocket(0, 2, Wire.LOOPBACK);
    }

    // What the game receives in PARTYBOARD_ONLINE_BARRIER.
    String barrier() {
        return server.getLocalPort() + ":" + secret;
    }

    void waitReady(BooleanSupplier cancelled, BooleanSupplier gameAlive, long timeoutMs) throws IOException {
        long deadline = System.currentTimeMillis() + timeoutMs;
        boolean seenAlive = false;
        server.setSoTimeout(250);
        byte[] expected = new byte[HELLO.length + 33];
        System.arraycopy(HELLO, 0, expected, 0, HELLO.length);
        System.arraycopy(secret.getBytes(StandardCharsets.US_ASCII), 0, expected, HELLO.length, 32);
        expected[expected.length - 1] = 'R';
        while (!closed) {
            if (cancelled.getAsBoolean()) {
                throw new IOException("cancelled");
            }
            if (System.currentTimeMillis() > deadline) {
                throw new IOException(Msg.s("Le jeu n'a pas terminé son chargement après deux minutes.",
                    "The game did not finish loading within two minutes."));
            }
            // Like the PC's Process.HasExited: a game that closed while
            // loading is said at once rather than after two minutes.
            if (gameAlive.getAsBoolean()) {
                seenAlive = true;
            } else if (seenAlive) {
                throw new IOException(Msg.s("Le jeu s'est fermé pendant le chargement. Vérifiez votre disque et relancez le salon.",
                    "The game closed while loading. Check your disc and start the lobby again."));
            }
            Socket s;
            try {
                s = server.accept();
            } catch (SocketTimeoutException e) {
                continue;
            }
            try {
                s.setSoTimeout(5000);
                byte[] hello = Wire.readFully(s.getInputStream(), expected.length);
                if (!Wire.equal(hello, expected)) {
                    s.close();
                    continue;
                }
                s.setSoTimeout(0);
                game = s;
                Wire.closeQuietly(server);
                return;
            } catch (IOException e) {
                Wire.closeQuietly(s);
            }
        }
        throw new IOException("closed");
    }

    void go() throws IOException {
        Socket s = game;
        if (s == null) {
            throw new IOException(Msg.s("Le jeu s'est fermé avant le départ.", "The game closed before the start."));
        }
        OutputStream out = s.getOutputStream();
        out.write('G');
        out.flush();
    }

    // Blocks until the game process has gone.
    void waitExit() {
        Socket s = game;
        if (s == null) {
            return;
        }
        try {
            InputStream in = s.getInputStream();
            byte[] sink = new byte[64];
            while (in.read(sink) >= 0) {
                Arrays.fill(sink, (byte) 0);
            }
        } catch (IOException ignored) {
        }
    }

    void abort() {
        Socket s = game;
        if (s != null) {
            try {
                s.getOutputStream().write('C');
            } catch (IOException ignored) {
            }
        }
        close();
    }

    @Override
    public void close() {
        closed = true;
        Wire.closeQuietly(server);
        Wire.closeQuietly(game);
    }
}
