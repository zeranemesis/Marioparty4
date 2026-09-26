package com.mariopartyrd.partyboard.online;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

// tools/online/Connection.cs (GameDatagram, ReplayWindow and the packet checks
// Bridge and MeshRelay share). Time-sensitive game input travels on UDP, sealed
// with an HMAC keyed from the invitation token, so one lost Internet packet
// never blocks the next frame the way it would on the TLS channel.
final class Datagram {
    // include/port/netplay_transport.hpp: kNetplayPacketSize, kProtocolVersion.
    static final int PAYLOAD = 152;
    static final int PROTOCOL_VERSION = 7;
    static final int HEADER = 14;
    static final int TAG = 16;
    static final int SIZE = HEADER + PAYLOAD + TAG;

    private Datagram() {}

    static byte[] key(byte[] token) {
        return Wire.hash(token, "PartyBoard UDP v1".getBytes(StandardCharsets.US_ASCII));
    }

    static Mac mac(byte[] key) {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(key, "HmacSHA256"));
            return mac;
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static byte[] seal(Mac mac, int player, long sequence, byte[] payload) {
        if (payload == null || payload.length != PAYLOAD || player < 0 || player >= Lobby.MAX_SEATS) {
            throw new IllegalArgumentException("invalid game packet");
        }
        byte[] data = new byte[SIZE];
        data[0] = 'P';
        data[1] = 'B';
        data[2] = 'U';
        data[3] = '1';
        data[4] = 1;
        data[5] = (byte) player;
        ByteBuffer.wrap(data, 6, 8).order(ByteOrder.LITTLE_ENDIAN).putLong(sequence);
        System.arraycopy(payload, 0, data, HEADER, PAYLOAD);
        synchronized (mac) {
            mac.update(data, 0, HEADER + PAYLOAD);
            byte[] tag = mac.doFinal();
            System.arraycopy(tag, 0, data, HEADER + PAYLOAD, TAG);
        }
        return data;
    }

    // Returns the payload, or null for anything not authenticated as `player`.
    static byte[] open(Mac mac, int player, byte[] data, int length, long[] sequenceOut) {
        if (length != SIZE || data[0] != 'P' || data[1] != 'B' || data[2] != 'U' || data[3] != '1' || data[4] != 1
            || (data[5] & 0xff) != player)
        {
            return null;
        }
        byte[] expected;
        synchronized (mac) {
            mac.update(data, 0, HEADER + PAYLOAD);
            expected = mac.doFinal();
        }
        if (!Wire.equal(Arrays.copyOf(expected, TAG), Arrays.copyOfRange(data, HEADER + PAYLOAD, SIZE))) {
            return null;
        }
        sequenceOut[0] = ByteBuffer.wrap(data, 6, 8).order(ByteOrder.LITTLE_ENDIAN).getLong();
        return Arrays.copyOfRange(data, HEADER, HEADER + PAYLOAD);
    }

    // A game packet the native engine wrote for `player` (src/port/netplay_transport.cpp).
    static boolean isGamePacket(byte[] b, int length, int player) {
        return length == PAYLOAD && b[0] == 'P' && b[1] == 'B' && b[2] == 'R' && b[3] == 'B'
            && (b[4] & 0xff) == (PROTOCOL_VERSION >> 8) && (b[5] & 0xff) == (PROTOCOL_VERSION & 0xff)
            && b[6] >= 1 && b[6] <= 3 && (b[7] & 0xff) == player;
    }

    static byte[] heartbeat(int player, long stamp) {
        byte[] b = new byte[PAYLOAD];
        b[0] = 'P';
        b[1] = 'B';
        b[2] = 'H';
        b[3] = 'B';
        b[4] = 1;
        b[5] = (byte) player;
        b[6] = 1;
        ByteBuffer.wrap(b, 8, 8).order(ByteOrder.LITTLE_ENDIAN).putLong(stamp);
        return b;
    }

    static boolean isHeartbeat(byte[] b, int player) {
        return b.length == PAYLOAD && b[0] == 'P' && b[1] == 'B' && b[2] == 'H' && b[3] == 'B' && b[4] == 1
            && (b[5] & 0xff) == player && (b[6] == 1 || b[6] == 2);
    }

    static long heartbeatStamp(byte[] b) {
        return ByteBuffer.wrap(b, 8, 8).order(ByteOrder.LITTLE_ENDIAN).getLong();
    }

    // Replay protection, one sliding window of 64 per sender.
    static final class ReplayWindow {
        private final long[] highest = new long[Lobby.MAX_SEATS];
        private final long[] window = new long[Lobby.MAX_SEATS];
        private final boolean[] seen = new boolean[Lobby.MAX_SEATS];

        synchronized boolean isNewest(int seat, long sequence) {
            return !seen[seat] || Long.compareUnsigned(sequence, highest[seat]) > 0;
        }

        synchronized boolean accept(int seat, long sequence) {
            if (seat < 0 || seat >= Lobby.MAX_SEATS) {
                return false;
            }
            if (!seen[seat]) {
                seen[seat] = true;
                highest[seat] = sequence;
                window[seat] = 1;
                return true;
            }
            if (Long.compareUnsigned(sequence, highest[seat]) > 0) {
                long d = sequence - highest[seat];
                window[seat] = Long.compareUnsigned(d, 64) >= 0 ? 1 : (window[seat] << d) | 1;
                highest[seat] = sequence;
                return true;
            }
            long behind = highest[seat] - sequence;
            if (Long.compareUnsigned(behind, 64) >= 0) {
                return false;
            }
            long bit = 1L << behind;
            if ((window[seat] & bit) != 0) {
                return false;
            }
            window[seat] |= bit;
            return true;
        }
    }
}
