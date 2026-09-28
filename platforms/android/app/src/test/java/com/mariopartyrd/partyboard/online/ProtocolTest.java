package com.mariopartyrd.partyboard.online;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Test;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

import javax.crypto.Mac;
import javax.net.ssl.SSLSocket;

// The online lobby's wire protocol, without a phone: tools/online/Tests.cs covers
// the same ground for the PC companion.
public class ProtocolTest {
    private static Invitation invitation(InetAddress address, int port) {
        Invitation i = new Invitation();
        i.address = address;
        i.port = port;
        i.expiresSeconds = System.currentTimeMillis() / 1000L + 30 * 60;
        i.fingerprint = Wire.random(16);
        i.token = Wire.random(16);
        i.maxPlayers = 2;
        return i;
    }

    private static InetAddress ip(int a, int b, int c, int d) throws IOException {
        return InetAddress.getByAddress(new byte[] { (byte) a, (byte) b, (byte) c, (byte) d });
    }

    @Test
    public void invitationRoundTripsInThePcFormat() throws Exception {
        Invitation i = invitation(ip(203, 1, 113, 7), 43033);
        i.localAddress = ip(192, 168, 1, 20);
        i.localPort = 43033;
        i.maxPlayers = 3;
        String text = i.encode();
        assertTrue(text.startsWith("PB4."));
        assertEquals(72, text.length());
        Invitation d = Invitation.decode(text, false);
        assertEquals(i.address, d.address);
        assertEquals(i.port, d.port);
        assertEquals(i.expiresSeconds, d.expiresSeconds);
        assertArrayEquals(i.fingerprint, d.fingerprint);
        assertArrayEquals(i.token, d.token);
        assertEquals(i.localAddress, d.localAddress);
        assertEquals(3, d.maxPlayers);
        assertTrue(d.hasLocalPath());
        assertFalse(d.localOnly());
    }

    @Test
    public void invitationIsFoundInsideAChatMessage() throws Exception {
        String text = invitation(ip(203, 1, 113, 7), 5000).encode();
        assertEquals(text, Invitation.extract("Rejoins mon salon : " + text + " !"));
        assertEquals(5000, Invitation.decode("Rejoins : " + text, false).port);
    }

    @Test
    public void sameWifiInvitationNeedsItsLocalPath() throws Exception {
        Invitation lan = invitation(Invitation.any(), 0);
        lan.localAddress = ip(192, 168, 1, 20);
        lan.localPort = 40000;
        Invitation d = Invitation.decode(lan.encode(), false);
        assertTrue(d.localOnly());
        assertTrue(d.hasLocalPath());

        Invitation nowhere = invitation(Invitation.any(), 0);
        try {
            Invitation.decode(nowhere.encode(), false);
            fail("an invitation with no address at all was accepted");
        } catch (IOException expected) {
        }
    }

    @Test
    public void invitationRefusesPrivatePublicAddressOlderFormatsAndExpiry() throws Exception {
        try {
            Invitation.decode(invitation(ip(192, 168, 1, 1), 5000).encode(), false);
            fail("a private public address was accepted");
        } catch (IOException expected) {
        }
        try {
            Invitation.decode("PB3.AAAA", false);
            fail("an older invitation was accepted");
        } catch (IOException expected) {
            assertTrue(expected.getMessage().length() > 0);
        }
        Invitation old = invitation(ip(203, 1, 113, 7), 5000);
        old.expiresSeconds = System.currentTimeMillis() / 1000L - 10;
        try {
            Invitation.decode(old.encode(), false);
            fail("an expired invitation was accepted");
        } catch (IOException expected) {
        }
        // A non-private local address is dropped, not trusted.
        Invitation odd = invitation(ip(203, 1, 113, 7), 5000);
        odd.localAddress = ip(8, 8, 8, 8);
        odd.localPort = 5000;
        assertFalse(Invitation.decode(odd.encode(), false).hasLocalPath());
    }

    @Test
    public void datagramsAreSealedPerSeatAndTamperEvident() {
        Mac mac = Datagram.mac(Datagram.key(Wire.random(16)));
        byte[] payload = Datagram.heartbeat(1, 1234);
        byte[] packet = Datagram.seal(mac, 1, 77, payload);
        long[] sequence = new long[1];
        assertArrayEquals(payload, Datagram.open(mac, 1, packet, packet.length, sequence));
        assertEquals(77, sequence[0]);
        assertNull("wrong seat", Datagram.open(mac, 0, packet, packet.length, sequence));
        packet[Datagram.HEADER + 3] ^= 1;
        assertNull("tampered payload", Datagram.open(mac, 1, packet, packet.length, sequence));
        Mac other = Datagram.mac(Datagram.key(Wire.random(16)));
        byte[] foreign = Datagram.seal(other, 1, 1, payload);
        assertNull("other lobby's key", Datagram.open(mac, 1, foreign, foreign.length, sequence));
    }

    @Test
    public void gamePacketsMustComeFromTheirOwnSeat() {
        byte[] b = new byte[Datagram.PAYLOAD];
        b[0] = 'P';
        b[1] = 'B';
        b[2] = 'R';
        b[3] = 'B';
        b[4] = 0;
        b[5] = 7;
        b[6] = 1;
        b[7] = 1;
        assertTrue(Datagram.isGamePacket(b, b.length, 1));
        assertFalse(Datagram.isGamePacket(b, b.length, 0));
        b[5] = 6; // older engine protocol
        assertFalse(Datagram.isGamePacket(b, b.length, 1));
    }

    @Test
    public void replayWindowIsPerSeat() {
        Datagram.ReplayWindow w = new Datagram.ReplayWindow();
        assertTrue(w.accept(1, 10));
        assertFalse("repeat", w.accept(1, 10));
        assertTrue("late but new", w.accept(1, 5));
        assertTrue("another seat has its own window", w.accept(2, 10));
        assertTrue(w.accept(1, 200));
        assertFalse("more than 64 behind", w.accept(1, 100));
    }

    private static Lobby.PlayerInfo player(String name, byte[] hash) throws IOException {
        return new Lobby.PlayerInfo(name, hash, 470366104L, Lobby.ModSet.EMPTY);
    }

    @Test
    public void playerInfoKeepsThePcEncoding() throws Exception {
        byte[] hash = Wire.random(32);
        Lobby.PlayerInfo p = player("Valentin", hash);
        Lobby.PlayerInfo d = Lobby.PlayerInfo.decode(p.encode());
        assertEquals("Valentin", d.name);
        assertTrue(p.sameDisc(d));
        assertTrue(p.sameMods(d));
        byte[] extra = Arrays.copyOf(p.encode(), p.encode().length + 1);
        try {
            Lobby.PlayerInfo.decode(extra);
            fail("a longer announcement was read short");
        } catch (IOException expected) {
        }
        try {
            player("", hash);
            fail("an empty nickname was accepted");
        } catch (IOException expected) {
        }
    }

    private static final class Pair {
        Lobby host;
        Lobby guest;
        final List<String> events = new ArrayList<>();
        byte[] hostAttempt;
        byte[] guestAttempt;
    }

    private static Pair pair(byte[] hostDisc, byte[] guestDisc) throws IOException {
        Pair p = new Pair();
        p.host = new Lobby(true, player("Host", hostDisc), (seat, payload) -> p.guest.receive(payload),
            a -> { p.hostAttempt = a; p.events.add("host prepare"); }, a -> p.events.add("host commit"), () -> {});
        p.guest = new Lobby(false, player("Guest", guestDisc), (seat, payload) -> p.host.receive(payload),
            a -> { p.guestAttempt = a; p.events.add("guest prepare"); }, a -> p.events.add("guest commit"), () -> {});
        p.host.announce();
        p.guest.announce();
        return p;
    }

    @Test
    public void twoPlayersStartOnlyWhenBothHaveLoaded() throws Exception {
        byte[] disc = Wire.random(32);
        Pair p = pair(disc, disc);
        assertTrue(p.host.canStart());
        assertFalse("a guest never starts", p.guest.canStart());
        p.host.start();
        assertEquals(Lobby.Phase.PREPARING, p.guest.phase());
        assertArrayEquals(p.hostAttempt, p.guestAttempt);
        p.host.loaded(p.hostAttempt);
        assertFalse("commit waits for the guest", p.events.contains("host commit"));
        p.guest.loaded(p.guestAttempt);
        assertTrue(p.events.contains("host commit"));
        assertTrue(p.events.contains("guest commit"));
        assertEquals(Lobby.Phase.RUNNING, p.host.phase());
        assertEquals(Lobby.Phase.RUNNING, p.guest.phase());

        p.guest.localGameExited();
        assertEquals(Lobby.Ending.LOCAL_GAME_CLOSED, p.guest.ending());
        assertEquals(Lobby.Ending.REMOTE_GAME_CLOSED, p.host.ending());
    }

    @Test
    public void differentDiscsBlockTheStart() throws Exception {
        Pair p = pair(Wire.random(32), Wire.random(32));
        assertFalse(p.host.canStart());
        assertFalse(p.host.discMatches());
        try {
            p.host.start();
            fail("started with different discs");
        } catch (IOException expected) {
        }
    }

    @Test
    public void aGuestCannotStartOrForgeTheCommit() throws Exception {
        byte[] disc = Wire.random(32);
        Pair p = pair(disc, disc);
        try {
            p.guest.start();
            fail("a guest started the game");
        } catch (IOException expected) {
        }
        try {
            p.host.receive(Lobby.command(5, Wire.random(16)));
            fail("the host accepted a start from its guest");
        } catch (IOException expected) {
        }
    }

    @Test
    public void tlsHandshakePinsTheInvitationCertificate() throws Exception {
        Wire.Identity identity = Wire.certificate();
        byte[] build = Wire.random(32);
        Invitation invite = invitation(ip(203, 1, 113, 7), 1);
        invite.fingerprint = identity.fingerprint;
        try (ServerSocket server = new ServerSocket(0, 4, Wire.LOOPBACK)) {
            // Accepted: same build, pinned certificate.
            CompletableFuture<Wire.Accepted> accepted = CompletableFuture.supplyAsync(() -> {
                try {
                    return Wire.server(server.accept(), identity, invite, build);
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
            byte[] clientId = Wire.random(16);
            Socket tcp = new Socket();
            tcp.connect(new InetSocketAddress(Wire.LOOPBACK, server.getLocalPort()), 2000);
            SSLSocket client = Wire.client(tcp, invite, build, 1, clientId);
            Wire.Accepted a = accepted.get(20, TimeUnit.SECONDS);
            assertEquals(1, a.channel);
            assertArrayEquals(clientId, a.clientId);
            client.getOutputStream().write(42);
            client.getOutputStream().flush();
            assertEquals(42, a.ssl.getInputStream().read());
            client.close();
            a.ssl.close();

            // Refused: another build is told so by name.
            CompletableFuture<Void> refused = CompletableFuture.runAsync(() -> {
                try {
                    Wire.server(server.accept(), identity, invite, build).ssl.close();
                } catch (IOException expected) {
                }
            });
            Socket other = new Socket();
            other.connect(new InetSocketAddress(Wire.LOOPBACK, server.getLocalPort()), 2000);
            try {
                Wire.client(other, invite, Wire.random(32), 0, clientId);
                fail("a different build was admitted");
            } catch (Wire.Refused expected) {
                assertNotNull(expected.getMessage());
            }
            refused.get(20, TimeUnit.SECONDS);

            // A stranger's certificate fails the pin.
            Invitation wrongPin = invitation(ip(203, 1, 113, 7), 1);
            wrongPin.token = invite.token;
            CompletableFuture<Void> stranger = CompletableFuture.runAsync(() -> {
                try {
                    Wire.server(server.accept(), identity, invite, build).ssl.close();
                } catch (IOException expected) {
                }
            });
            Socket third = new Socket();
            third.connect(new InetSocketAddress(Wire.LOOPBACK, server.getLocalPort()), 2000);
            try {
                Wire.client(third, wrongPin, build, 0, clientId);
                fail("an unpinned certificate was trusted");
            } catch (IOException expected) {
                assertFalse(expected instanceof Wire.Refused);
            }
            stranger.get(20, TimeUnit.SECONDS);
        }
    }
}
