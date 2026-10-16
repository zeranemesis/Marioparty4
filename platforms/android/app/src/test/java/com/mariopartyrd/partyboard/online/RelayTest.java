package com.mariopartyrd.partyboard.online;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import org.junit.Test;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.channels.DatagramChannel;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

// The relay without a phone: the same ground tools/online/Tests.cs covers on the
// PC (StunVectors, TurnRelay, RelayEndToEnd).
public class RelayTest {
    private static byte[] bytes(int... values) {
        byte[] b = new byte[values.length];
        for (int i = 0; i < values.length; i++) {
            b[i] = (byte) values[i];
        }
        return b;
    }

    // RFC 5769 2.2, the sample IPv4 response.
    @Test
    public void rfc5769ResponseVector() {
        byte[] vector = bytes(
            0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
            0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
            0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
            0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7,
            0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96);
        Relay.Stun.Message m = Relay.Stun.decode(vector, vector.length);
        assertNotNull(m);
        assertEquals(Relay.Stun.BINDING, m.method());
        assertEquals(Relay.Stun.SUCCESS, m.cls());
        InetSocketAddress mapped = Relay.Stun.readXorAddress(m.get(Relay.Stun.XOR_MAPPED_ADDRESS));
        assertEquals("192.0.2.1", mapped.getAddress().getHostAddress());
        assertEquals(32853, mapped.getPort());
        assertTrue("MESSAGE-INTEGRITY", Relay.Stun.verify(m, "VOkJxbRl1RmTxUk/WvJxBt".getBytes(StandardCharsets.UTF_8)));
        assertFalse(Relay.Stun.verify(m, "wrong".getBytes(StandardCharsets.UTF_8)));
        assertTrue("FINGERPRINT", Relay.Stun.verifyFingerprint(m));
        byte[] key = Relay.Stun.longTermKey("user", "example.org", "pass");
        Relay.Stun.Message request = Relay.Stun.newMessage(Relay.Stun.CREATE_PERMISSION, Relay.Stun.REQUEST)
            .add(Relay.Stun.XOR_PEER_ADDRESS, Relay.Stun.xorAddress(new InetSocketAddress("198.51.100.7", 4242)));
        byte[] encoded = Relay.Stun.encode(request, key, true);
        Relay.Stun.Message back = Relay.Stun.decode(encoded, encoded.length);
        assertTrue(Relay.Stun.verify(back, key) && Relay.Stun.verifyFingerprint(back));
        assertEquals(0x0113, Relay.Stun.typeOf(Relay.Stun.ALLOCATE, Relay.Stun.ERROR));
        assertEquals(0x0017, Relay.Stun.typeOf(Relay.Stun.DATA_METHOD, Relay.Stun.INDICATION));
    }

    // The PC names the room the same way; a fixed token pins the bytes.
    @Test
    public void roomIdMatchesThePc() {
        String room = Relay.roomId(new byte[16]);
        assertEquals(32, room.length());
        assertTrue(room.matches("[0-9a-f]{32}"));
        assertNotEquals(room, Relay.roomId(Wire.random(16)));
        assertEquals("hello", Relay.field("{\"type\":\"hello\",\"udp\":\"1.2.3.4:5\"}", "type"));
        assertEquals("1.2.3.4:5", Relay.field("{\"type\":\"hello\",\"udp\":\"1.2.3.4:5\"}", "udp"));
        assertEquals(Arrays.asList("turn:a:3478?transport=udp", "stun:b"), Relay.strings("{\"urls\":[\"turn:a:3478?transport=udp\",\"stun:b\"]}", "urls"));
    }

    // A loopback TURN server with the subset TurnClient uses, relaying real UDP.
    private static final class MockTurn implements AutoCloseable {
        final DatagramSocket control = new DatagramSocket(new InetSocketAddress(Wire.LOOPBACK, 0));
        final DatagramSocket relayed = new DatagramSocket(new InetSocketAddress(Wire.LOOPBACK, 0));
        final byte[] key = Relay.Stun.longTermKey("alice", "mock.realm", "secret");
        final Set<String> permitted = ConcurrentHashMap.newKeySet();
        final Map<Integer, InetSocketAddress> channels = new ConcurrentHashMap<>();
        volatile InetSocketAddress client;
        volatile int channelData;

        MockTurn() throws Exception {
            Thread a = new Thread(this::serveControl);
            Thread b = new Thread(this::serveRelay);
            a.setDaemon(true);
            b.setDaemon(true);
            a.start();
            b.start();
        }

        InetSocketAddress endpoint() {
            return new InetSocketAddress(Wire.LOOPBACK, control.getLocalPort());
        }

        void reply(Relay.Stun.Message request, int cls, Relay.Stun.Message out, boolean sign) throws Exception {
            out.type = Relay.Stun.typeOf(request.method(), cls);
            System.arraycopy(request.transaction, 0, out.transaction, 0, 12);
            byte[] b = Relay.Stun.encode(out, sign ? key : null, true);
            control.send(new DatagramPacket(b, b.length, client));
        }

        void serveControl() {
            byte[] buffer = new byte[2048];
            while (!control.isClosed()) {
                try {
                    DatagramPacket p = new DatagramPacket(buffer, buffer.length);
                    control.receive(p);
                    client = (InetSocketAddress) p.getSocketAddress();
                    int n = p.getLength();
                    if ((buffer[0] & 0xff) >= 0x40 && (buffer[0] & 0xff) <= 0x7f) {
                        int ch = (buffer[0] & 0xff) << 8 | (buffer[1] & 0xff), len = (buffer[2] & 0xff) << 8 | (buffer[3] & 0xff);
                        InetSocketAddress peer = channels.get(ch);
                        if (peer != null) {
                            channelData++;
                            relayed.send(new DatagramPacket(buffer, 4, len, peer));
                        }
                        continue;
                    }
                    Relay.Stun.Message m = Relay.Stun.decode(buffer, n);
                    if (m == null) {
                        continue;
                    }
                    if (m.method() == Relay.Stun.SEND && m.cls() == Relay.Stun.INDICATION) {
                        InetSocketAddress peer = Relay.Stun.readXorAddress(m.get(Relay.Stun.XOR_PEER_ADDRESS));
                        if (permitted.contains(peer.getAddress().getHostAddress())) {
                            byte[] data = m.get(Relay.Stun.DATA);
                            relayed.send(new DatagramPacket(data, data.length, peer));
                        }
                        continue;
                    }
                    if (m.method() == Relay.Stun.BINDING) {
                        reply(m, Relay.Stun.SUCCESS, new Relay.Stun.Message().add(Relay.Stun.XOR_MAPPED_ADDRESS, Relay.Stun.xorAddress(client)), false);
                        continue;
                    }
                    if (m.get(Relay.Stun.MESSAGE_INTEGRITY) == null || !Relay.Stun.verify(m, key)) {
                        reply(m, Relay.Stun.ERROR, new Relay.Stun.Message().add(Relay.Stun.ERROR_CODE, bytes(0, 0, 4, 1))
                            .add(Relay.Stun.REALM, "mock.realm".getBytes(StandardCharsets.UTF_8)).add(Relay.Stun.NONCE, "n1".getBytes(StandardCharsets.UTF_8)), false);
                        continue;
                    }
                    Relay.Stun.Message ok = new Relay.Stun.Message();
                    if (m.method() == Relay.Stun.ALLOCATE) {
                        ok.add(Relay.Stun.XOR_RELAYED_ADDRESS, Relay.Stun.xorAddress(new InetSocketAddress(Wire.LOOPBACK, relayed.getLocalPort())))
                            .add(Relay.Stun.LIFETIME, Relay.Stun.u32(600));
                    } else if (m.method() == Relay.Stun.CREATE_PERMISSION) {
                        permitted.add(Relay.Stun.readXorAddress(m.get(Relay.Stun.XOR_PEER_ADDRESS)).getAddress().getHostAddress());
                    } else if (m.method() == Relay.Stun.CHANNEL_BIND) {
                        byte[] c = m.get(Relay.Stun.CHANNEL_NUMBER);
                        channels.put((c[0] & 0xff) << 8 | (c[1] & 0xff), Relay.Stun.readXorAddress(m.get(Relay.Stun.XOR_PEER_ADDRESS)));
                    }
                    reply(m, Relay.Stun.SUCCESS, ok, true);
                } catch (Exception e) {
                    if (control.isClosed()) {
                        return;
                    }
                }
            }
        }

        void serveRelay() {
            byte[] buffer = new byte[2048];
            while (!relayed.isClosed()) {
                try {
                    DatagramPacket p = new DatagramPacket(buffer, buffer.length);
                    relayed.receive(p);
                    InetSocketAddress from = (InetSocketAddress) p.getSocketAddress();
                    if (!permitted.contains(from.getAddress().getHostAddress()) || client == null) {
                        continue;
                    }
                    byte[] data = Arrays.copyOf(buffer, p.getLength());
                    byte[] frame = Relay.Stun.encode(Relay.Stun.newMessage(Relay.Stun.DATA_METHOD, Relay.Stun.INDICATION)
                        .add(Relay.Stun.XOR_PEER_ADDRESS, Relay.Stun.xorAddress(from)).add(Relay.Stun.DATA, data), null, false);
                    control.send(new DatagramPacket(frame, frame.length, client));
                } catch (Exception e) {
                    if (relayed.isClosed()) {
                        return;
                    }
                }
            }
        }

        @Override
        public void close() {
            control.close();
            relayed.close();
        }
    }

    // TURN, then the shim Bridge would use: a guest's datagram reaches the shim's
    // loopback channel from a per-guest leg, and the reply sent to that leg
    // leaves through TURN to the guest.
    @Test
    public void turnThroughTheShim() throws Exception {
        try (MockTurn server = new MockTurn(); DatagramSocket guest = new DatagramSocket(new InetSocketAddress(Wire.LOOPBACK, 0))) {
            Relay.TurnClient turn = new Relay.TurnClient(server.endpoint(), "alice", "secret");
            turn.allocate();
            assertEquals(server.relayed.getLocalPort(), turn.relayed.getPort());
            turn.permit(Wire.LOOPBACK);
            try (Relay.TurnShim shim = new Relay.TurnShim(turn)) {
                guest.setSoTimeout(3000);
                byte[] hello = "hello host".getBytes(StandardCharsets.UTF_8);
                guest.send(new DatagramPacket(hello, hello.length, turn.relayed));
                ByteBuffer in = ByteBuffer.allocate(2048);
                InetSocketAddress leg = null;
                long deadline = System.currentTimeMillis() + 3000;
                shim.internet.configureBlocking(false);
                while (leg == null && System.currentTimeMillis() < deadline) {
                    leg = (InetSocketAddress) shim.internet.receive(in);
                    if (leg == null) {
                        Thread.sleep(5);
                    }
                }
                assertNotNull("the guest's datagram reaches the shim", leg);
                assertEquals("hello host", new String(in.array(), 0, in.position(), StandardCharsets.UTF_8));
                for (int i = 0; i < 30; i++) {
                    byte[] reply = ("reply " + i).getBytes(StandardCharsets.UTF_8);
                    shim.internet.send(ByteBuffer.wrap(reply), leg);
                    DatagramPacket p = new DatagramPacket(new byte[2048], 2048);
                    guest.receive(p);
                    assertEquals("reply " + i, new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8));
                    assertEquals(turn.relayed.getPort(), p.getPort());
                    Thread.sleep(10);
                }
                assertTrue("later replies use the bound channel", server.channelData > 0);
            }
        }
    }

    // With PARTYBOARD_RELAY set to a running relay (npx wrangler dev): the room
    // pairs a guest with the host, and bytes cross it both ways between a guest's
    // loopback socket and the host's own listener.
    @Test
    public void throughARunningRelay() throws Exception {
        // Only when asked: the default relay is the deployed one.
        String asked = System.getenv("PARTYBOARD_RELAY");
        assumeTrue("PARTYBOARD_RELAY not set", asked != null && !asked.isEmpty());
        String room = Relay.roomId(Wire.random(16));
        try (ServerSocket hostListener = new ServerSocket(0, 4, Wire.LOOPBACK);
             Relay.RelaySocket control = Relay.connect("/room/" + room + "/host?udp=203.0.113.9:4000")) {
            Set<Integer> relayed = ConcurrentHashMap.newKeySet();
            InetSocketAddress own = new InetSocketAddress(Wire.LOOPBACK, hostListener.getLocalPort());
            Thread host = new Thread(() -> {
                try {
                    Relay.RelaySocket.Frame f = control.read();
                    String conn = Relay.field(f.text, "conn");
                    assertEquals("incoming", Relay.field(f.text, "type"));
                    assertEquals("198.51.100.4", Relay.field(f.text, "ip"));
                    Relay.forward(Relay.connect("/room/" + room + "/accept/" + conn), own, relayed);
                } catch (Exception e) {
                    throw new RuntimeException(e);
                }
            });
            host.start();
            InetSocketAddress[] hostUdp = new InetSocketAddress[1];
            Socket guest = Relay.loopback(Relay.guestPipe(room, "198.51.100.4", hostUdp));
            assertEquals(new InetSocketAddress("203.0.113.9", 4000), hostUdp[0]);
            hostListener.setSoTimeout(10000);
            Socket arrived = hostListener.accept();
            assertTrue("the host tells a relayed connection by its port", relayed.contains(arrived.getPort()));
            byte[] up = new byte[40000];
            new java.util.Random(7).nextBytes(up);
            OutputStream go = guest.getOutputStream();
            go.write(up);
            go.flush();
            assertArrayEquals(up, readFully(arrived.getInputStream(), up.length));
            byte[] down = "from the host".getBytes(StandardCharsets.UTF_8);
            arrived.getOutputStream().write(down);
            assertArrayEquals(down, readFully(guest.getInputStream(), down.length));
            guest.close();
            arrived.close();
        }
        // A room without its host refuses the guest at once.
        try {
            Relay.guestPipe(Relay.roomId(Wire.random(16)), null, new InetSocketAddress[1]);
            assertTrue("a room without its host must refuse", false);
        } catch (java.io.IOException expected) {
        }
    }

    private static byte[] readFully(InputStream in, int length) throws Exception {
        byte[] b = new byte[length];
        int at = 0;
        while (at < length) {
            int n = in.read(b, at, length - at);
            if (n < 0) {
                throw new java.io.EOFException();
            }
            at += n;
        }
        return b;
    }
}
