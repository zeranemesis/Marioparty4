package com.mariopartyrd.partyboard.online;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.channels.DatagramChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.CRC32;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

// The relay for a host whose box cannot be opened (CGNAT, no UPnP): the same
// protocol as tools/online/Relay.cs, so a phone and a PC meet in the same room.
// The lobby's TLS runs unchanged through the room's WebSockets -- loopback
// sockets carry it to and from them -- and the game datagrams go through TURN.
final class Relay {
    private Relay() {}

    // The deployed Worker. PARTYBOARD_RELAY overrides it (the JVM tests); empty
    // turns the relay off, and the lobby behaves as before.
    static final String DEFAULT_URL = "";

    static String baseUrl() {
        String v = System.getenv("PARTYBOARD_RELAY");
        String url = v != null ? v : DEFAULT_URL;
        while (url.endsWith("/")) {
            url = url.substring(0, url.length() - 1);
        }
        return url;
    }

    static boolean available() {
        return !baseUrl().isEmpty();
    }

    // Named by a hash of the invitation token, never the token itself: the
    // same 32 hex characters the PC computes.
    static String roomId(byte[] token) {
        MessageDigest sha = Wire.sha256();
        sha.update("PBRELAY1".getBytes(StandardCharsets.US_ASCII));
        sha.update(token);
        byte[] hash = sha.digest();
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < 16; i++) {
            s.append(String.format("%02x", hash[i] & 0xff));
        }
        return s.toString();
    }

    // The fields the relay's small JSON messages carry; no general parser is
    // needed, and org.json is only a stub in the JVM tests.
    static String field(String json, String name) {
        Matcher m = Pattern.compile("\"" + Pattern.quote(name) + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(json);
        return m.find() ? m.group(1).replace("\\/", "/").replace("\\\"", "\"").replace("\\\\", "\\") : null;
    }

    static List<String> strings(String json, String name) {
        List<String> out = new ArrayList<>();
        Matcher m = Pattern.compile("\"" + Pattern.quote(name) + "\"\\s*:\\s*\\[([^\\]]*)\\]").matcher(json);
        if (m.find()) {
            Matcher item = Pattern.compile("\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(m.group(1));
            while (item.find()) {
                out.add(item.group(1).replace("\\/", "/"));
            }
        }
        return out;
    }

    static InetSocketAddress endpoint(String text) {
        if (text == null) {
            return null;
        }
        int colon = text.lastIndexOf(':');
        if (colon <= 0) {
            return null;
        }
        try {
            int port = Integer.parseInt(text.substring(colon + 1));
            InetAddress a = InetAddress.getByName(text.substring(0, colon));
            return port > 0 && port < 65536 && a instanceof Inet4Address ? new InetSocketAddress(a, port) : null;
        } catch (NumberFormatException | IOException e) {
            return null;
        }
    }

    static final class Credentials {
        final String username;
        final String credential;
        final InetSocketAddress server;

        Credentials(String username, String credential, InetSocketAddress server) {
            this.username = username;
            this.credential = credential;
            this.server = server;
        }
    }

    static Credentials turnCredentials() throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(baseUrl() + "/turn").openConnection();
        try {
            c.setRequestMethod("POST");
            c.setConnectTimeout(8000);
            c.setReadTimeout(8000);
            c.setDoOutput(true);
            c.setRequestProperty("User-Agent", "PartyBoard-Online");
            c.setRequestProperty("Content-Type", "application/json");
            c.getOutputStream().write("{}".getBytes(StandardCharsets.UTF_8));
            if (c.getResponseCode() != 200) {
                throw new IOException("relay turn " + c.getResponseCode());
            }
            String json = new String(readAll(c.getInputStream()), StandardCharsets.UTF_8);
            String username = field(json, "username");
            String credential = field(json, "credential");
            String url = null;
            for (String u : strings(json, "urls")) {
                if (u.startsWith("turn:") && u.contains("transport=udp") && (url == null || u.contains(":3478"))) {
                    url = u;
                }
            }
            if (username == null || credential == null || url == null) {
                throw new IOException("relay turn: incomplete");
            }
            String hostPort = url.substring(5).split("\\?")[0];
            int colon = hostPort.lastIndexOf(':');
            InetAddress address = null;
            for (InetAddress a : InetAddress.getAllByName(hostPort.substring(0, colon))) {
                if (a instanceof Inet4Address) {
                    address = a;
                    break;
                }
            }
            if (address == null) {
                throw new IOException("relay turn: no IPv4");
            }
            return new Credentials(username, credential, new InetSocketAddress(address, Integer.parseInt(hostPort.substring(colon + 1))));
        } finally {
            c.disconnect();
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] chunk = new byte[4096];
        int n;
        while ((n = in.read(chunk)) > 0) {
            out.write(chunk, 0, n);
        }
        return out.toByteArray();
    }

    static RelaySocket connect(String path) throws IOException {
        String base = baseUrl();
        String socket = base.startsWith("https:") ? "wss:" + base.substring(6) : base.startsWith("http:") ? "ws:" + base.substring(5) : base;
        return RelaySocket.open(socket + path, 10000);
    }

    // Guest side: one channel through the room. The host's TURN address comes in
    // "hello"; the TLS may start once the host has taken the connection ("ready").
    static RelaySocket guestPipe(String room, String self, InetSocketAddress[] hostUdp) throws IOException {
        RelaySocket pipe = connect("/room/" + room + "/guest" + (self != null ? "?ip=" + self : ""));
        try {
            pipe.setTimeout(10000);
            while (true) {
                RelaySocket.Frame f = pipe.read();
                if (f == null) {
                    throw new IOException(Msg.s(
                        "Votre ami n'est pas joignable, ni directement ni par le relais en ligne. Vérifiez qu'il a laissé son salon ouvert.",
                        "Your friend cannot be reached, directly or through the online relay. Check that their lobby is still open."));
                }
                if (f.text == null) {
                    continue;
                }
                String type = field(f.text, "type");
                if ("hello".equals(type)) {
                    hostUdp[0] = endpoint(field(f.text, "udp"));
                } else if ("ready".equals(type)) {
                    pipe.setTimeout(0);
                    return pipe;
                }
            }
        } catch (IOException | RuntimeException e) {
            pipe.close();
            throw e;
        }
    }

    // This socket's public IPv4 as a STUN server sees it (stun.cloudflare.com
    // unless told otherwise): the address the host must permit on its TURN
    // allocation. Null when it cannot be learnt; the room then uses the address
    // the WebSocket came from.
    static String publicAddress(DatagramChannel channel, InetSocketAddress stunServer) {
        try {
            InetSocketAddress server = stunServer;
            if (server == null) {
                for (InetAddress a : InetAddress.getAllByName("stun.cloudflare.com")) {
                    if (a instanceof Inet4Address) {
                        server = new InetSocketAddress(a, 3478);
                        break;
                    }
                }
            }
            if (server == null) {
                return null;
            }
            Stun.Message request = Stun.newMessage(Stun.BINDING, Stun.REQUEST);
            byte[] bytes = Stun.encode(request, null, false);
            boolean blocking = channel.isBlocking();
            channel.configureBlocking(false);
            try {
                ByteBuffer buffer = ByteBuffer.allocate(1500);
                for (int attempt = 0; attempt < 2; attempt++) {
                    channel.send(ByteBuffer.wrap(bytes), server);
                    long deadline = System.nanoTime() + 1_500_000_000L;
                    while (System.nanoTime() < deadline) {
                        buffer.clear();
                        if (channel.receive(buffer) == null) {
                            Thread.sleep(10);
                            continue;
                        }
                        Stun.Message m = Stun.decode(buffer.array(), buffer.position());
                        if (m != null && Wire.equal(m.transaction, request.transaction)) {
                            InetSocketAddress mapped = Stun.readXorAddress(m.get(Stun.XOR_MAPPED_ADDRESS));
                            if (mapped != null) {
                                return mapped.getAddress().getHostAddress();
                            }
                        }
                    }
                }
            } finally {
                channel.configureBlocking(blocking);
            }
            return null;
        } catch (IOException | InterruptedException | RuntimeException e) {
            return null;
        }
    }

    // Copies a socket and a relay WebSocket into each other until either ends.
    static void pump(Socket socket, RelaySocket ws) {
        Thread up = new Thread(() -> {
            byte[] chunk = new byte[16384];
            try {
                InputStream in = socket.getInputStream();
                int n;
                while ((n = in.read(chunk)) > 0) {
                    ws.sendBinary(chunk, 0, n);
                }
            } catch (IOException ignored) {
            } finally {
                ws.close();
                Wire.closeQuietly(socket);
            }
        }, "relay-up");
        Thread down = new Thread(() -> {
            try {
                OutputStream out = socket.getOutputStream();
                while (true) {
                    RelaySocket.Frame f = ws.read();
                    if (f == null) {
                        break;
                    }
                    if (f.binary != null) {
                        out.write(f.binary);
                        out.flush();
                    }
                }
            } catch (IOException ignored) {
            } finally {
                ws.close();
                Wire.closeQuietly(socket);
            }
        }, "relay-down");
        up.setDaemon(true);
        down.setDaemon(true);
        up.start();
        down.start();
    }

    // Guest side: a loopback socket whose bytes travel through the WebSocket, so
    // the existing SSLSocket handshake runs on it unchanged.
    static Socket loopback(RelaySocket ws) throws IOException {
        try (ServerSocket listener = new ServerSocket(0, 1, Wire.LOOPBACK)) {
            Socket near = new Socket(Wire.LOOPBACK, listener.getLocalPort());
            listener.setSoTimeout(5000);
            Socket far = listener.accept();
            far.setTcpNoDelay(true);
            near.setTcpNoDelay(true);
            pump(far, ws);
            return near;
        }
    }

    // Host side: a guest's relayed connection delivered to this host's own
    // listener, as if it had come over TCP. The local port is put in `relayed`
    // before connecting -- the listener sees it as the connection's remote port,
    // and that is how accept() tells a relayed guest from a direct one.
    static void forward(RelaySocket ws, InetSocketAddress listener, Set<Integer> relayed) throws IOException {
        Socket socket = new Socket();
        socket.bind(new InetSocketAddress(listener.getAddress(), 0));
        relayed.add(socket.getLocalPort());
        socket.connect(listener, 5000);
        socket.setTcpNoDelay(true);
        pump(socket, ws);
    }

    // A WebSocket client (RFC 6455), as much as the relay needs: one connection,
    // text and binary messages, ping, close. Over TLS with the system's trust and
    // hostname check for wss, plain for a local wrangler dev.
    static final class RelaySocket implements Closeable {
        static final class Frame {
            final String text;
            final byte[] binary;

            Frame(String text, byte[] binary) {
                this.text = text;
                this.binary = binary;
            }
        }

        private final Socket socket;
        private final DataInputStream in;
        private final OutputStream out;
        private final SecureRandom random = new SecureRandom();
        private volatile boolean closed;

        private RelaySocket(Socket socket) throws IOException {
            this.socket = socket;
            in = new DataInputStream(socket.getInputStream());
            out = socket.getOutputStream();
        }

        static RelaySocket open(String url, int timeoutMs) throws IOException {
            URI uri = URI.create(url);
            boolean secure = "wss".equals(uri.getScheme());
            String host = uri.getHost();
            int port = uri.getPort() > 0 ? uri.getPort() : secure ? 443 : 80;
            Socket raw = new Socket();
            raw.connect(new InetSocketAddress(host, port), timeoutMs);
            raw.setTcpNoDelay(true);
            raw.setSoTimeout(timeoutMs);
            Socket socket = raw;
            if (secure) {
                SSLSocket ssl = (SSLSocket) ((SSLSocketFactory) SSLSocketFactory.getDefault()).createSocket(raw, host, port, true);
                ssl.startHandshake();
                if (!HttpsURLConnection.getDefaultHostnameVerifier().verify(host, ssl.getSession())) {
                    ssl.close();
                    throw new IOException("relay: certificate does not match " + host);
                }
                socket = ssl;
            }
            RelaySocket ws = new RelaySocket(socket);
            try {
                ws.handshake(host + (uri.getPort() > 0 ? ":" + port : ""), uri.getRawPath() + (uri.getRawQuery() != null ? "?" + uri.getRawQuery() : ""));
                socket.setSoTimeout(0);
                return ws;
            } catch (IOException | RuntimeException e) {
                ws.close();
                throw e;
            }
        }

        private void handshake(String hostHeader, String path) throws IOException {
            byte[] nonce = new byte[16];
            random.nextBytes(nonce);
            String key = Base64.getEncoder().encodeToString(nonce);
            String request = "GET " + path + " HTTP/1.1\r\nHost: " + hostHeader + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                + "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: PartyBoard-Online\r\n\r\n";
            out.write(request.getBytes(StandardCharsets.US_ASCII));
            out.flush();
            String status = line();
            if (!status.startsWith("HTTP/1.1 101")) {
                throw new IOException("relay: " + status);
            }
            String accept = null;
            for (String header = line(); !header.isEmpty(); header = line()) {
                int colon = header.indexOf(':');
                if (colon > 0 && header.substring(0, colon).trim().equalsIgnoreCase("Sec-WebSocket-Accept")) {
                    accept = header.substring(colon + 1).trim();
                }
            }
            MessageDigest sha1;
            try {
                sha1 = MessageDigest.getInstance("SHA-1");
            } catch (java.security.NoSuchAlgorithmException e) {
                throw new IOException(e);
            }
            String expected = Base64.getEncoder().encodeToString(
                sha1.digest((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").getBytes(StandardCharsets.US_ASCII)));
            if (!expected.equals(accept)) {
                throw new IOException("relay: bad handshake");
            }
        }

        private String line() throws IOException {
            StringBuilder s = new StringBuilder();
            while (true) {
                int c = in.read();
                if (c < 0) {
                    throw new IOException("relay: closed during handshake");
                }
                if (c == '\n') {
                    break;
                }
                if (c != '\r') {
                    s.append((char) c);
                }
                if (s.length() > 8192) {
                    throw new IOException("relay: header too long");
                }
            }
            return s.toString();
        }

        void setTimeout(int ms) throws IOException {
            socket.setSoTimeout(ms);
        }

        // The next text or binary message; null once the relay has closed.
        Frame read() throws IOException {
            ByteArrayOutputStream message = new ByteArrayOutputStream();
            int type = -1;
            while (true) {
                int b0, b1;
                try {
                    b0 = in.readUnsignedByte();
                    b1 = in.readUnsignedByte();
                } catch (java.io.EOFException e) {
                    closed = true;
                    return null;
                } catch (SocketTimeoutException e) {
                    throw new IOException(Msg.s("Le relais ne répond plus.", "The relay stopped answering."), e);
                }
                boolean fin = (b0 & 0x80) != 0;
                int opcode = b0 & 0x0F;
                long length = b1 & 0x7F;
                if (length == 126) {
                    length = in.readUnsignedShort();
                } else if (length == 127) {
                    length = in.readLong();
                }
                if (length < 0 || length > (1 << 20)) {
                    throw new IOException("relay: frame too large");
                }
                byte[] mask = null;
                if ((b1 & 0x80) != 0) {
                    mask = new byte[4];
                    in.readFully(mask);
                }
                byte[] payload = new byte[(int) length];
                in.readFully(payload);
                if (mask != null) {
                    for (int i = 0; i < payload.length; i++) {
                        payload[i] ^= mask[i & 3];
                    }
                }
                if (opcode == 0x8) {
                    closed = true;
                    try {
                        frame(0x8, payload, 0, Math.min(payload.length, 2));
                    } catch (IOException ignored) {
                    }
                    return null;
                }
                if (opcode == 0x9) {
                    frame(0xA, payload, 0, payload.length);
                    continue;
                }
                if (opcode == 0xA) {
                    continue;
                }
                if (opcode == 0x1 || opcode == 0x2) {
                    type = opcode;
                }
                message.write(payload, 0, payload.length);
                if (fin) {
                    byte[] bytes = message.toByteArray();
                    return type == 0x1 ? new Frame(new String(bytes, StandardCharsets.UTF_8), null) : new Frame(null, bytes);
                }
            }
        }

        void sendBinary(byte[] data, int offset, int length) throws IOException {
            frame(0x2, data, offset, length);
        }

        // Clients mask every frame (RFC 6455 5.3).
        private synchronized void frame(int opcode, byte[] data, int offset, int length) throws IOException {
            if (closed && opcode != 0x8) {
                throw new IOException("relay: closed");
            }
            ByteArrayOutputStream f = new ByteArrayOutputStream(length + 14);
            f.write(0x80 | opcode);
            if (length < 126) {
                f.write(0x80 | length);
            } else if (length < 65536) {
                f.write(0x80 | 126);
                f.write(length >> 8);
                f.write(length);
            } else {
                f.write(0x80 | 127);
                for (int i = 7; i >= 0; i--) {
                    f.write((int) ((long) length >> (8 * i)));
                }
            }
            byte[] mask = new byte[4];
            random.nextBytes(mask);
            f.write(mask, 0, 4);
            for (int i = 0; i < length; i++) {
                f.write(data[offset + i] ^ mask[i & 3]);
            }
            out.write(f.toByteArray());
            out.flush();
        }

        boolean closed() {
            return closed;
        }

        @Override
        public void close() {
            if (!closed) {
                closed = true;
                try {
                    frame(0x8, new byte[] { 0x03, (byte) 0xE8 }, 0, 2);
                } catch (IOException ignored) {
                }
            }
            Wire.closeQuietly(socket);
        }
    }

    // STUN (RFC 5389) with the TURN additions (RFC 8656) the relay uses, IPv4
    // only: the same codec as tools/online/Relay.cs.
    static final class Stun {
        private Stun() {}

        static final int COOKIE = 0x2112A442;
        static final int BINDING = 0x001, ALLOCATE = 0x003, REFRESH = 0x004, SEND = 0x006, DATA_METHOD = 0x007,
            CREATE_PERMISSION = 0x008, CHANNEL_BIND = 0x009;
        static final int REQUEST = 0x000, INDICATION = 0x010, SUCCESS = 0x100, ERROR = 0x110;
        static final int USERNAME = 0x0006, MESSAGE_INTEGRITY = 0x0008, ERROR_CODE = 0x0009, CHANNEL_NUMBER = 0x000C,
            LIFETIME = 0x000D, XOR_PEER_ADDRESS = 0x0012, DATA = 0x0013, REALM = 0x0014, NONCE = 0x0015,
            XOR_RELAYED_ADDRESS = 0x0016, REQUESTED_TRANSPORT = 0x0019, XOR_MAPPED_ADDRESS = 0x0020, SOFTWARE = 0x8022,
            FINGERPRINT = 0x8028;
        private static final SecureRandom RANDOM = new SecureRandom();

        static final class Message {
            int type;
            final byte[] transaction = new byte[12];
            final List<Map.Entry<Integer, byte[]>> attributes = new ArrayList<>();
            int integrityOffset = -1;
            byte[] raw;

            int method() {
                return type & 0x000F | (type & 0x00E0) >> 1 | (type & 0x3E00) >> 2;
            }

            int cls() {
                return type & 0x0110;
            }

            byte[] get(int attribute) {
                for (Map.Entry<Integer, byte[]> a : attributes) {
                    if (a.getKey() == attribute) {
                        return a.getValue();
                    }
                }
                return null;
            }

            Message add(int attribute, byte[] value) {
                attributes.add(new java.util.AbstractMap.SimpleEntry<>(attribute, value));
                return this;
            }
        }

        static int typeOf(int method, int cls) {
            return (method & 0x000F) | (method & 0x0070) << 1 | (method & 0x0F80) << 2 | cls;
        }

        static Message newMessage(int method, int cls) {
            Message m = new Message();
            m.type = typeOf(method, cls);
            RANDOM.nextBytes(m.transaction);
            return m;
        }

        private static void put16(byte[] b, int p, int v) {
            b[p] = (byte) (v >> 8);
            b[p + 1] = (byte) v;
        }

        private static int get16(byte[] b, int p) {
            return (b[p] & 0xff) << 8 | (b[p + 1] & 0xff);
        }

        private static int get32(byte[] b, int p) {
            return (b[p] & 0xff) << 24 | (b[p + 1] & 0xff) << 16 | (b[p + 2] & 0xff) << 8 | (b[p + 3] & 0xff);
        }

        static byte[] encode(Message m, byte[] key, boolean fingerprint) {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            for (Map.Entry<Integer, byte[]> a : m.attributes) {
                byte[] head = new byte[4];
                put16(head, 0, a.getKey());
                put16(head, 2, a.getValue().length);
                body.write(head, 0, 4);
                body.write(a.getValue(), 0, a.getValue().length);
                body.write(new byte[(4 - a.getValue().length % 4) % 4], 0, (4 - a.getValue().length % 4) % 4);
            }
            byte[] b = body.toByteArray();
            byte[] message = new byte[20 + b.length];
            put16(message, 0, m.type);
            put16(message, 2, b.length);
            ByteBuffer.wrap(message, 4, 4).putInt(COOKIE);
            System.arraycopy(m.transaction, 0, message, 8, 12);
            System.arraycopy(b, 0, message, 20, b.length);
            if (key != null) {
                put16(message, 2, b.length + 24);
                byte[] mac = hmac(key, message, message.length);
                byte[] signed = new byte[message.length + 24];
                System.arraycopy(message, 0, signed, 0, message.length);
                put16(signed, message.length, MESSAGE_INTEGRITY);
                put16(signed, message.length + 2, 20);
                System.arraycopy(mac, 0, signed, message.length + 4, 20);
                message = signed;
            }
            if (fingerprint) {
                put16(message, 2, message.length - 20 + 8);
                CRC32 crc = new CRC32();
                crc.update(message, 0, message.length);
                byte[] withCrc = new byte[message.length + 8];
                System.arraycopy(message, 0, withCrc, 0, message.length);
                put16(withCrc, message.length, FINGERPRINT);
                put16(withCrc, message.length + 2, 4);
                ByteBuffer.wrap(withCrc, message.length + 4, 4).putInt((int) crc.getValue() ^ 0x5354554E);
                message = withCrc;
            }
            return message;
        }

        static Message decode(byte[] b, int length) {
            if (length < 20 || (b[0] & 0xC0) != 0 || get32(b, 4) != COOKIE) {
                return null;
            }
            int size = get16(b, 2);
            if (size % 4 != 0 || 20 + size > length) {
                return null;
            }
            Message m = new Message();
            m.type = get16(b, 0);
            System.arraycopy(b, 8, m.transaction, 0, 12);
            m.raw = new byte[20 + size];
            System.arraycopy(b, 0, m.raw, 0, 20 + size);
            int p = 20;
            while (p + 4 <= 20 + size) {
                int type = get16(b, p), len = get16(b, p + 2);
                if (p + 4 + len > 20 + size) {
                    return null;
                }
                if (type == MESSAGE_INTEGRITY) {
                    m.integrityOffset = p;
                }
                byte[] value = new byte[len];
                System.arraycopy(b, p + 4, value, 0, len);
                m.add(type, value);
                p += 4 + len + (4 - len % 4) % 4;
            }
            return m;
        }

        static boolean verify(Message m, byte[] key) {
            if (m.integrityOffset < 0 || m.raw == null) {
                return false;
            }
            byte[] signed = new byte[m.integrityOffset];
            System.arraycopy(m.raw, 0, signed, 0, signed.length);
            put16(signed, 2, m.integrityOffset - 20 + 24);
            byte[] mac = hmac(key, signed, signed.length);
            byte[] stated = new byte[20];
            System.arraycopy(m.raw, m.integrityOffset + 4, stated, 0, 20);
            return MessageDigest.isEqual(mac, stated);
        }

        static boolean verifyFingerprint(Message m) {
            byte[] stated = m.get(FINGERPRINT);
            if (stated == null || stated.length != 4 || m.raw.length < 8) {
                return false;
            }
            CRC32 crc = new CRC32();
            crc.update(m.raw, 0, m.raw.length - 8);
            return ((int) crc.getValue() ^ 0x5354554E) == get32(stated, 0);
        }

        private static byte[] hmac(byte[] key, byte[] data, int length) {
            try {
                Mac mac = Mac.getInstance("HmacSHA1");
                mac.init(new SecretKeySpec(key, "HmacSHA1"));
                mac.update(data, 0, length);
                return mac.doFinal();
            } catch (java.security.GeneralSecurityException e) {
                throw new IllegalStateException(e);
            }
        }

        static byte[] longTermKey(String username, String realm, String password) {
            try {
                return MessageDigest.getInstance("MD5").digest((username + ":" + realm + ":" + password).getBytes(StandardCharsets.UTF_8));
            } catch (java.security.NoSuchAlgorithmException e) {
                throw new IllegalStateException(e);
            }
        }

        static byte[] xorAddress(InetSocketAddress e) {
            byte[] a = e.getAddress().getAddress();
            if (a.length != 4) {
                throw new IllegalArgumentException("IPv4 only");
            }
            byte[] v = new byte[8];
            v[1] = 1;
            put16(v, 2, e.getPort() ^ (COOKIE >>> 16));
            byte[] c = ByteBuffer.allocate(4).putInt(COOKIE).array();
            for (int i = 0; i < 4; i++) {
                v[4 + i] = (byte) (a[i] ^ c[i]);
            }
            return v;
        }

        static InetSocketAddress readXorAddress(byte[] v) {
            if (v == null || v.length < 8 || v[1] != 1) {
                return null;
            }
            byte[] c = ByteBuffer.allocate(4).putInt(COOKIE).array();
            byte[] a = new byte[4];
            for (int i = 0; i < 4; i++) {
                a[i] = (byte) (v[4 + i] ^ c[i]);
            }
            try {
                return new InetSocketAddress(InetAddress.getByAddress(a), get16(v, 2) ^ (COOKIE >>> 16));
            } catch (java.net.UnknownHostException e) {
                return null;
            }
        }

        static int errorCode(byte[] v) {
            return v == null || v.length < 4 ? 0 : (v[2] & 7) * 100 + (v[3] & 0xff);
        }

        static byte[] u32(int value) {
            return ByteBuffer.allocate(4).putInt(value).array();
        }

        static int readU32(byte[] v) {
            return v == null || v.length < 4 ? 0 : get32(v, 0);
        }
    }

    // A TURN allocation: the host's Internet-facing address when its box cannot be
    // opened. Received datagrams go to receiver; the same as TurnClient in Relay.cs.
    static final class TurnClient implements Closeable {
        interface Receiver {
            void datagram(InetSocketAddress peer, byte[] payload);
        }

        private final DatagramSocket server;
        private final String username;
        private final String password;
        private String realm;
        private byte[] nonce;
        private volatile byte[] key;
        volatile InetSocketAddress relayed;
        volatile Receiver receiver;
        private final Map<String, CompletableFuture<Stun.Message>> pending = new ConcurrentHashMap<>();
        private final Map<String, Integer> channelOf = new ConcurrentHashMap<>();
        private final Map<Integer, InetSocketAddress> peerOf = new ConcurrentHashMap<>();
        private final Set<String> binding = ConcurrentHashMap.newKeySet();
        private final Set<InetAddress> permitted = ConcurrentHashMap.newKeySet();
        private final AtomicInteger nextChannel = new AtomicInteger(0x3FFF);
        private volatile boolean closed;
        private int lifetime = 600;

        TurnClient(InetSocketAddress turnServer, String username, String password) throws IOException {
            this.username = username;
            this.password = password;
            server = new DatagramSocket();
            server.connect(turnServer);
            Thread reader = new Thread(this::readLoop, "turn-read");
            reader.setDaemon(true);
            reader.start();
        }

        private static String key(byte[] transaction) {
            return Base64.getEncoder().encodeToString(transaction);
        }

        private static String key(InetSocketAddress e) {
            return e.getAddress().getHostAddress() + ":" + e.getPort();
        }

        private Stun.Message transact(Stun.Message request, boolean sign) throws IOException {
            CompletableFuture<Stun.Message> done = new CompletableFuture<>();
            pending.put(key(request.transaction), done);
            try {
                byte[] bytes = Stun.encode(request, sign ? key : null, true);
                long wait = 500;
                for (int attempt = 0; attempt < 6 && !closed; attempt++, wait = Math.min(wait * 2, 4000)) {
                    server.send(new DatagramPacket(bytes, bytes.length));
                    try {
                        return done.get(wait, TimeUnit.MILLISECONDS);
                    } catch (java.util.concurrent.TimeoutException ignored) {
                    } catch (InterruptedException | java.util.concurrent.ExecutionException e) {
                        throw new IOException(e);
                    }
                }
                throw new IOException("relay: TURN server does not answer");
            } finally {
                pending.remove(key(request.transaction));
            }
        }

        private interface Fill {
            void into(Stun.Message m);
        }

        private Stun.Message authenticated(int method, Fill fill) throws IOException {
            for (int attempt = 0; attempt < 2; attempt++) {
                Stun.Message request = Stun.newMessage(method, Stun.REQUEST);
                fill.into(request);
                request.add(Stun.USERNAME, username.getBytes(StandardCharsets.UTF_8))
                    .add(Stun.REALM, realm.getBytes(StandardCharsets.UTF_8)).add(Stun.NONCE, nonce);
                Stun.Message reply = transact(request, true);
                if (reply.cls() == Stun.SUCCESS) {
                    return reply;
                }
                int code = Stun.errorCode(reply.get(Stun.ERROR_CODE));
                if (code == 438 && reply.get(Stun.NONCE) != null) {
                    nonce = reply.get(Stun.NONCE);
                    continue;
                }
                throw new IOException("relay: TURN refused (" + code + ")");
            }
            throw new IOException("relay: TURN refused");
        }

        void allocate() throws IOException {
            Stun.Message probe = Stun.newMessage(Stun.ALLOCATE, Stun.REQUEST).add(Stun.REQUESTED_TRANSPORT, new byte[] { 17, 0, 0, 0 });
            Stun.Message challenge = transact(probe, false);
            if (challenge.cls() != Stun.ERROR || Stun.errorCode(challenge.get(Stun.ERROR_CODE)) != 401
                || challenge.get(Stun.REALM) == null || challenge.get(Stun.NONCE) == null) {
                throw new IOException("relay: TURN offers no authentication");
            }
            realm = new String(challenge.get(Stun.REALM), StandardCharsets.UTF_8);
            nonce = challenge.get(Stun.NONCE);
            key = Stun.longTermKey(username, realm, password);
            Stun.Message reply = authenticated(Stun.ALLOCATE, r -> r.add(Stun.REQUESTED_TRANSPORT, new byte[] { 17, 0, 0, 0 }));
            relayed = Stun.readXorAddress(reply.get(Stun.XOR_RELAYED_ADDRESS));
            if (relayed == null) {
                throw new IOException("relay: TURN gave no address");
            }
            int granted = Stun.readU32(reply.get(Stun.LIFETIME));
            if (granted >= 60) {
                lifetime = granted;
            }
            Thread refresher = new Thread(this::refreshLoop, "turn-refresh");
            refresher.setDaemon(true);
            refresher.start();
        }

        // An allocation lives `lifetime` seconds, a permission five minutes and a
        // channel binding ten: all three are refreshed well before.
        private void refreshLoop() {
            long period = Math.min(120, lifetime / 3) * 1000L;
            while (!closed) {
                try {
                    Thread.sleep(period);
                    if (closed) {
                        return;
                    }
                    authenticated(Stun.REFRESH, r -> r.add(Stun.LIFETIME, Stun.u32(lifetime)));
                    for (InetAddress a : permitted) {
                        createPermission(a);
                    }
                    for (Map.Entry<Integer, InetSocketAddress> c : peerOf.entrySet()) {
                        bind(c.getValue(), c.getKey());
                    }
                } catch (InterruptedException e) {
                    return;
                } catch (IOException ignored) {
                }
            }
        }

        void permit(InetAddress address) throws IOException {
            if (permitted.add(address)) {
                createPermission(address);
            }
        }

        private void createPermission(InetAddress address) throws IOException {
            authenticated(Stun.CREATE_PERMISSION, r -> r.add(Stun.XOR_PEER_ADDRESS, Stun.xorAddress(new InetSocketAddress(address, 0))));
        }

        private void bind(InetSocketAddress peer, int channel) throws IOException {
            authenticated(Stun.CHANNEL_BIND, r -> r.add(Stun.CHANNEL_NUMBER, new byte[] { (byte) (channel >> 8), (byte) channel, 0, 0 })
                .add(Stun.XOR_PEER_ADDRESS, Stun.xorAddress(peer)));
        }

        void send(byte[] data, int offset, int length, InetSocketAddress to) throws IOException {
            Integer channel = channelOf.get(key(to));
            if (channel != null) {
                byte[] frame = new byte[4 + length];
                frame[0] = (byte) (channel >> 8);
                frame[1] = (byte) (int) channel;
                frame[2] = (byte) (length >> 8);
                frame[3] = (byte) length;
                System.arraycopy(data, offset, frame, 4, length);
                server.send(new DatagramPacket(frame, frame.length));
                return;
            }
            byte[] payload = new byte[length];
            System.arraycopy(data, offset, payload, 0, length);
            byte[] bytes = Stun.encode(Stun.newMessage(Stun.SEND, Stun.INDICATION).add(Stun.XOR_PEER_ADDRESS, Stun.xorAddress(to)).add(Stun.DATA, payload), null, false);
            server.send(new DatagramPacket(bytes, bytes.length));
            if (binding.add(key(to))) {
                Thread t = new Thread(() -> {
                    int next = nextChannel.incrementAndGet();
                    if (next > 0x7FFF) {
                        return;
                    }
                    try {
                        bind(to, next);
                        peerOf.put(next, to);
                        channelOf.put(key(to), next);
                    } catch (IOException e) {
                        binding.remove(key(to));
                    }
                }, "turn-bind");
                t.setDaemon(true);
                t.start();
            }
        }

        private void readLoop() {
            byte[] buffer = new byte[2048];
            while (!closed) {
                DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                try {
                    server.receive(packet);
                } catch (IOException e) {
                    if (closed) {
                        return;
                    }
                    continue;
                }
                int n = packet.getLength();
                if (n >= 4 && (buffer[0] & 0xff) >= 0x40 && (buffer[0] & 0xff) <= 0x7F) {
                    int channel = (buffer[0] & 0xff) << 8 | (buffer[1] & 0xff), length = (buffer[2] & 0xff) << 8 | (buffer[3] & 0xff);
                    InetSocketAddress peer = peerOf.get(channel);
                    if (4 + length <= n && peer != null) {
                        deliver(peer, java.util.Arrays.copyOfRange(buffer, 4, 4 + length));
                    }
                    continue;
                }
                Stun.Message m = Stun.decode(buffer, n);
                if (m == null) {
                    continue;
                }
                if (m.method() == Stun.DATA_METHOD && m.cls() == Stun.INDICATION) {
                    InetSocketAddress peer = Stun.readXorAddress(m.get(Stun.XOR_PEER_ADDRESS));
                    byte[] payload = m.get(Stun.DATA);
                    if (peer != null && payload != null) {
                        deliver(peer, payload);
                    }
                    continue;
                }
                CompletableFuture<Stun.Message> waiting = pending.get(key(m.transaction));
                if (waiting != null) {
                    byte[] k = key;
                    if (m.cls() == Stun.SUCCESS && k != null && !Stun.verify(m, k)) {
                        continue;
                    }
                    waiting.complete(m);
                }
            }
        }

        private void deliver(InetSocketAddress peer, byte[] payload) {
            Receiver r = receiver;
            if (r != null) {
                r.datagram(peer, payload);
            }
        }

        @Override
        public void close() {
            if (closed) {
                return;
            }
            closed = true;
            try {
                if (key != null) {
                    Stun.Message bye = Stun.newMessage(Stun.REFRESH, Stun.REQUEST).add(Stun.LIFETIME, Stun.u32(0))
                        .add(Stun.USERNAME, username.getBytes(StandardCharsets.UTF_8))
                        .add(Stun.REALM, realm.getBytes(StandardCharsets.UTF_8)).add(Stun.NONCE, nonce);
                    byte[] bytes = Stun.encode(bye, key, true);
                    server.send(new DatagramPacket(bytes, bytes.length));
                }
            } catch (IOException ignored) {
            }
            server.close();
        }
    }

    // Host side: gives Bridge an ordinary loopback DatagramChannel (internet) and
    // carries what it sends to each guest through TURN, and back. Bridge then
    // learns the guest by its loopback leg exactly as it would by a real address.
    static final class TurnShim implements Closeable {
        final DatagramChannel internet;
        private final TurnClient turn;
        private final Map<String, DatagramSocket> legs = new ConcurrentHashMap<>();
        private volatile boolean closed;

        TurnShim(TurnClient turn) throws IOException {
            this.turn = turn;
            internet = DatagramChannel.open();
            internet.bind(new InetSocketAddress(Wire.LOOPBACK, 0));
            turn.receiver = this::fromTurn;
        }

        private void fromTurn(InetSocketAddress peer, byte[] payload) {
            if (closed) {
                return;
            }
            try {
                DatagramSocket leg = leg(peer);
                leg.send(new DatagramPacket(payload, payload.length, internet.getLocalAddress()));
            } catch (IOException ignored) {
            }
        }

        private DatagramSocket leg(InetSocketAddress peer) throws IOException {
            String k = peer.getAddress().getHostAddress() + ":" + peer.getPort();
            DatagramSocket leg = legs.get(k);
            if (leg != null) {
                return leg;
            }
            DatagramSocket created = new DatagramSocket(new InetSocketAddress(Wire.LOOPBACK, 0));
            DatagramSocket raced = ((ConcurrentHashMap<String, DatagramSocket>) legs).putIfAbsent(k, created);
            if (raced != null) {
                created.close();
                return raced;
            }
            Thread t = new Thread(() -> {
                byte[] buffer = new byte[2048];
                while (!closed) {
                    DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                    try {
                        created.receive(packet);
                        turn.send(buffer, 0, packet.getLength(), peer);
                    } catch (IOException e) {
                        if (closed || created.isClosed()) {
                            return;
                        }
                    }
                }
            }, "turn-leg");
            t.setDaemon(true);
            t.start();
            return created;
        }

        @Override
        public void close() {
            closed = true;
            for (DatagramSocket leg : legs.values()) {
                leg.close();
            }
            turn.close();
        }
    }
}
