package com.mariopartyrd.partyboard.online;

import java.io.IOException;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Base64;

// tools/online/Connection.cs (Invitation). Same "PB4." text, same 49 bytes, so
// the format stays one format: public address and port, expiry, certificate
// fingerprint, token, the host's address on its own network, player count.
//
// One addition for phones: a lobby whose box refused every automatic mapping
// can still be joined from the same Wi-Fi. Such an invitation carries 0.0.0.0
// and port 0 as its public address and only the local path; a PC build never
// produces it, and would reject it, which is right since the two cannot play
// together anyway (the build hashes differ).
final class Invitation {
    static final int MAX_SEATS = Lobby.MAX_SEATS;

    InetAddress address;
    int port;
    long expiresSeconds;
    InetAddress localAddress = any();
    int localPort;
    byte[] fingerprint;
    byte[] token;
    byte[] build;
    int maxPlayers = 2;

    static InetAddress any() {
        try {
            return InetAddress.getByAddress(new byte[4]);
        } catch (UnknownHostException e) {
            throw new IllegalStateException(e);
        }
    }

    long expiresMillis() {
        return expiresSeconds * 1000L;
    }

    boolean localOnly() {
        return port == 0 && address != null && address.isAnyLocalAddress();
    }

    boolean hasLocalPath() {
        return localPort > 0 && localAddress != null && !localAddress.isAnyLocalAddress()
            && !localAddress.equals(address);
    }

    String encode() throws IOException {
        if (fingerprint.length != 16 || token.length != 16 || maxPlayers < 2 || maxPlayers > MAX_SEATS) {
            throw new IOException(Msg.s("Invitation incompatible.", "Incompatible invitation."));
        }
        ByteBuffer b = ByteBuffer.allocate(49).order(ByteOrder.LITTLE_ENDIAN);
        b.put(address.getAddress());
        b.putShort((short) port);
        b.putInt((int) expiresSeconds);
        b.put(fingerprint);
        b.put(token);
        b.put((localAddress == null ? any() : localAddress).getAddress());
        b.putShort((short) localPort);
        b.put((byte) maxPlayers);
        return "PB4." + Base64.getEncoder().encodeToString(b.array()).replace('+', '-').replace('/', '_');
    }

    private static IOException incomplete() {
        return new IOException(Msg.s("L'invitation est incomplète. Copiez-la entièrement depuis le message de votre ami.",
            "The invitation is incomplete. Copy all of it from your friend's message."));
    }

    // The PB4 code inside a message pasted or shared from a chat app.
    static String extract(String text) {
        if (text == null) {
            return "";
        }
        int at = text.indexOf("PB4.");
        return at >= 0 && text.length() >= at + 72 ? text.substring(at, at + 72) : text.trim();
    }

    static Invitation decode(String text, boolean localTest) throws IOException {
        if (text == null) {
            throw incomplete();
        }
        text = text.trim();
        // Pasted from a chat, an invitation often arrives inside a sentence.
        int at = text.indexOf("PB4.");
        if (at > 0 && text.length() >= at + 72) {
            text = text.substring(at, at + 72);
        }
        if (text.startsWith("PB2.") || text.startsWith("PB3.")) {
            throw new IOException(Msg.s(
                "Cette invitation vient d'une version plus ancienne de Party Board. Mettez le jeu à jour et recréez le salon.",
                "This invitation comes from an older Party Board. Update the game and create the lobby again."));
        }
        if (text.length() != 72 || !text.startsWith("PB4.")) {
            throw incomplete();
        }
        byte[] data;
        try {
            data = Base64.getDecoder().decode(text.substring(4).replace('-', '+').replace('_', '/'));
        } catch (IllegalArgumentException e) {
            throw incomplete();
        }
        if (data.length != 49) {
            throw incomplete();
        }
        ByteBuffer r = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        Invitation i = new Invitation();
        byte[] ip = new byte[4];
        r.get(ip);
        i.address = Inet4Address.getByAddress(ip);
        i.port = r.getShort() & 0xffff;
        i.expiresSeconds = r.getInt() & 0xffffffffL;
        i.fingerprint = new byte[16];
        r.get(i.fingerprint);
        i.token = new byte[16];
        r.get(i.token);
        byte[] local = new byte[4];
        r.get(local);
        i.localAddress = Inet4Address.getByAddress(local);
        i.localPort = r.getShort() & 0xffff;
        i.maxPlayers = r.get() & 0xff;
        if (i.maxPlayers < 2 || i.maxPlayers > MAX_SEATS) {
            throw incomplete();
        }
        // The local address is deliberately not required to be public, but it
        // must be private, so an invitation cannot send the first attempt
        // anywhere else. The TLS pin makes a wrong machine harmless anyway.
        if (i.localPort != 0 && !Gateway.isPrivate(i.localAddress)) {
            i.localAddress = any();
            i.localPort = 0;
        }
        boolean publicOk = i.port != 0 && (localTest || Gateway.isPublic(i.address));
        if (!publicOk && !(i.localOnly() && i.hasLocalPath())) {
            throw incomplete();
        }
        long now = System.currentTimeMillis() / 1000L;
        if (i.expiresSeconds < now) {
            throw new IOException(Msg.s("Cette invitation a expiré. L'hôte doit recréer un salon.",
                "This invitation has expired. The host has to create a new lobby."));
        }
        if (i.expiresSeconds > now + 31 * 60) {
            throw incomplete();
        }
        return i;
    }
}
