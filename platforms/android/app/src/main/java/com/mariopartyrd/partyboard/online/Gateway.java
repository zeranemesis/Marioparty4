package com.mariopartyrd.partyboard.online;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.RouteInfo;
import android.net.wifi.WifiManager;
import android.util.Xml;

import org.xmlpull.v1.XmlPullParser;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.StringReader;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

// tools/online/Gateway.cs: finite leases only (PCP, then NAT-PMP, then UPnP),
// no external discovery service, no permanent mapping, nothing but the box on
// this network's own default route.
final class Gateway implements AutoCloseable {
    static final class Route {
        final InetAddress local;
        final InetAddress router;

        Route(InetAddress local, InetAddress router) {
            this.local = local;
            this.router = router;
        }

        // Windows asks which interface reaches the Internet; Android already
        // knows, it is the active network. Hosting needs Wi-Fi or Ethernet: a
        // mobile operator hands out an address behind its own shared NAT, which
        // no box mapping can open. Joining works from anywhere.
        static Route detect(Context context) throws IOException {
            ConnectivityManager cm = context.getSystemService(ConnectivityManager.class);
            Network network = cm == null ? null : cm.getActiveNetwork();
            if (network == null) {
                throw new IOException(Msg.s("Aucune connexion Internet disponible.", "No Internet connection available."));
            }
            NetworkCapabilities caps = cm.getNetworkCapabilities(network);
            if (caps != null && caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) {
                throw new IOException(Msg.s(
                    "Votre VPN empêche la préparation automatique. Mettez-le en pause, puis créez le salon.",
                    "Your VPN prevents the automatic setup. Pause it, then create the lobby."));
            }
            if (caps == null || !(caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                || caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)))
            {
                throw new IOException(Msg.s(
                    "Pour créer un salon, connectez le téléphone au Wi-Fi. Pour rejoindre un ami, les données mobiles suffisent.",
                    "To create a lobby, connect the phone to Wi-Fi. Mobile data is enough to join a friend."));
            }
            LinkProperties link = cm.getLinkProperties(network);
            InetAddress local = null;
            InetAddress router = null;
            if (link != null) {
                for (LinkAddress a : link.getLinkAddresses()) {
                    if (a.getAddress() instanceof Inet4Address && !a.getAddress().isLoopbackAddress()) {
                        local = a.getAddress();
                        break;
                    }
                }
                for (RouteInfo r : link.getRoutes()) {
                    if (r.isDefaultRoute() && r.getGateway() instanceof Inet4Address && !r.getGateway().isAnyLocalAddress()) {
                        router = r.getGateway();
                        break;
                    }
                }
            }
            if (local == null || router == null || !isPrivate(router)) {
                throw new IOException(Msg.s(
                    "Cette connexion ne donne pas accès à une box compatible. Essayez depuis votre Wi-Fi habituel.",
                    "This connection does not reach a compatible router. Try from your usual Wi-Fi."));
            }
            return new Route(local, router);
        }
    }

    static boolean isPrivate(InetAddress a) {
        if (!(a instanceof Inet4Address)) {
            return false;
        }
        byte[] b = a.getAddress();
        int b0 = b[0] & 0xff, b1 = b[1] & 0xff;
        return b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168);
    }

    static boolean isPublic(InetAddress a) {
        if (!(a instanceof Inet4Address) || isPrivate(a)) {
            return false;
        }
        byte[] b = a.getAddress();
        int b0 = b[0] & 0xff, b1 = b[1] & 0xff, b2 = b[2] & 0xff;
        return b0 != 0 && b0 != 127 && b0 < 224 && !(b0 == 100 && b1 >= 64 && b1 <= 127) && !(b0 == 169 && b1 == 254)
            && !(b0 == 192 && (b1 == 0 || b1 == 2)) && !(b0 == 198 && (b1 == 18 || b1 == 19 || (b1 == 51 && b2 == 100)))
            && !(b0 == 203 && b1 == 0 && b2 == 113);
    }

    private final Context context;
    private final Route route;
    private final int internalPort;
    private final byte[] nonce = Wire.random(12);
    private final boolean udp;
    private final int ipProtocol;
    private final int requestedExternalPort;
    private String method;
    private String service;
    private URI control;
    private int externalPort;
    InetAddress address;
    long lifetime;

    Gateway(Context context, Route route, int port, int requestedPort, boolean datagram) {
        this.context = context;
        this.route = route;
        internalPort = port;
        requestedExternalPort = requestedPort;
        externalPort = requestedPort;
        udp = datagram;
        ipProtocol = datagram ? 17 : 6;
    }

    int port() {
        return externalPort;
    }

    private static void put16(byte[] b, int p, int v) {
        b[p] = (byte) (v >> 8);
        b[p + 1] = (byte) v;
    }

    private static void put32(byte[] b, int p, long v) {
        b[p] = (byte) (v >> 24);
        b[p + 1] = (byte) (v >> 16);
        b[p + 2] = (byte) (v >> 8);
        b[p + 3] = (byte) v;
    }

    private static int u16(byte[] b, int p) {
        return ((b[p] & 0xff) << 8) | (b[p + 1] & 0xff);
    }

    private static long u32(byte[] b, int p) {
        return ((long) (b[p] & 0xff) << 24) | ((b[p + 1] & 0xff) << 16) | ((b[p + 2] & 0xff) << 8) | (b[p + 3] & 0xff);
    }

    private byte[] exchange(byte[] request) throws IOException {
        try (DatagramSocket u = new DatagramSocket(new InetSocketAddress(route.local, 0))) {
            u.connect(route.router, 5351);
            u.setSoTimeout(1400);
            for (int i = 0; i < 2; i++) {
                u.send(new DatagramPacket(request, request.length));
                byte[] buffer = new byte[1100];
                DatagramPacket reply = new DatagramPacket(buffer, buffer.length);
                try {
                    u.receive(reply);
                    return Arrays.copyOf(buffer, reply.getLength());
                } catch (SocketTimeoutException e) {
                    if (i == 1) {
                        throw e;
                    }
                }
            }
        }
        throw new IOException("router did not answer");
    }

    private void pcp(long life) throws IOException {
        byte[] q = new byte[60];
        q[0] = 2;
        q[1] = 1;
        put32(q, 4, life);
        q[18] = q[19] = (byte) 255;
        System.arraycopy(route.local.getAddress(), 0, q, 20, 4);
        System.arraycopy(nonce, 0, q, 24, 12);
        q[36] = (byte) ipProtocol;
        put16(q, 40, internalPort);
        put16(q, 42, externalPort);
        byte[] b = exchange(q);
        if (b.length != 60 || b[0] != 2 || (b[1] & 0xff) != 129 || b[3] != 0 || (b[36] & 0xff) != ipProtocol
            || u16(b, 40) != internalPort || !Wire.equal(Arrays.copyOfRange(b, 24, 36), nonce))
        {
            throw new IOException("PCP reply");
        }
        for (int i = 44; i < 54; i++) {
            if (b[i] != 0) {
                throw new IOException("PCP reply");
            }
        }
        if ((b[54] & 0xff) != 255 || (b[55] & 0xff) != 255 || u16(b, 42) == 0) {
            throw new IOException("PCP reply");
        }
        if (life == 0) {
            return;
        }
        externalPort = u16(b, 42);
        address = InetAddress.getByAddress(Arrays.copyOfRange(b, 56, 60));
        lifetime = u32(b, 4);
        method = "PCP";
        validateLease();
    }

    private void pmp(long life) throws IOException {
        if (life != 0 && address == null) {
            byte[] a = exchange(new byte[] { 0, 0 });
            if (a.length != 12 || a[0] != 0 || (a[1] & 0xff) != 128 || u16(a, 2) != 0) {
                throw new IOException("NAT-PMP address");
            }
            address = InetAddress.getByAddress(Arrays.copyOfRange(a, 8, 12));
            if (!isPublic(address)) {
                throw new IOException("router behind another network");
            }
        }
        int operation = udp ? 1 : 2;
        byte[] q = new byte[12];
        q[1] = (byte) operation;
        put16(q, 4, internalPort);
        put16(q, 6, externalPort);
        put32(q, 8, life);
        byte[] b = exchange(q);
        if (b.length != 16 || b[0] != 0 || (b[1] & 0xff) != 128 + operation || u16(b, 2) != 0
            || u16(b, 8) != internalPort || u16(b, 10) == 0)
        {
            throw new IOException("NAT-PMP reply");
        }
        if (life == 0) {
            return;
        }
        externalPort = u16(b, 10);
        lifetime = u32(b, 12);
        method = "PMP";
        validateLease();
    }

    private void validateLease() throws IOException {
        if (!isPublic(address) || lifetime < 30 || lifetime > 7200) {
            throw new IOException("lease not compatible");
        }
    }

    private boolean safeUrl(URI u) {
        if (u == null || !"http".equals(u.getScheme()) || u.getRawUserInfo() != null || u.getRawFragment() != null
            || u.getPort() <= 0 || u.getHost() == null)
        {
            return false;
        }
        String host = u.getHost();
        if (!host.matches("\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}")) {
            return false;
        }
        try {
            return InetAddress.getByName(host).equals(route.router);
        } catch (IOException e) {
            return false;
        }
    }

    // A tiny HTTP/1.1 client: the box is on this network, speaks plain HTTP,
    // and Android refuses cleartext through its URL stack by default.
    private static final class HttpResult {
        final int status;
        final String body;

        HttpResult(int status, String body) {
            this.status = status;
            this.body = body;
        }
    }

    private HttpResult http(URI uri, String action, String body) throws IOException {
        if (!safeUrl(uri)) {
            throw new IOException("router URL refused");
        }
        byte[] payload = null;
        StringBuilder request = new StringBuilder();
        String path = uri.getRawPath() == null || uri.getRawPath().isEmpty() ? "/" : uri.getRawPath();
        if (uri.getRawQuery() != null) {
            path += "?" + uri.getRawQuery();
        }
        if (action != null) {
            payload = ("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                + "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\""
                + service + "\">" + body + "</u:" + action + "></s:Body></s:Envelope>").getBytes(StandardCharsets.UTF_8);
            request.append("POST ").append(path).append(" HTTP/1.1\r\n");
            request.append("Content-Type: text/xml; charset=utf-8\r\n");
            request.append("SOAPAction: \"").append(service).append('#').append(action).append("\"\r\n");
            request.append("Content-Length: ").append(payload.length).append("\r\n");
        } else {
            request.append("GET ").append(path).append(" HTTP/1.1\r\n");
        }
        request.append("Host: ").append(uri.getHost()).append(':').append(uri.getPort()).append("\r\n");
        request.append("Connection: close\r\n\r\n");
        try (Socket s = new Socket()) {
            s.bind(new InetSocketAddress(route.local, 0));
            s.connect(new InetSocketAddress(uri.getHost(), uri.getPort()), 2500);
            s.setSoTimeout(2500);
            OutputStream out = s.getOutputStream();
            out.write(request.toString().getBytes(StandardCharsets.US_ASCII));
            if (payload != null) {
                out.write(payload);
            }
            out.flush();
            InputStream in = s.getInputStream();
            ByteArrayOutputStream all = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int n;
            while ((n = in.read(buffer)) > 0) {
                all.write(buffer, 0, n);
                if (all.size() > 131072 + 16384) {
                    throw new IOException("router reply too large");
                }
            }
            return parseResponse(all.toByteArray());
        }
    }

    private static HttpResult parseResponse(byte[] raw) throws IOException {
        int split = -1;
        for (int i = 0; i + 3 < raw.length; i++) {
            if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
                split = i;
                break;
            }
        }
        if (split < 0) {
            throw new IOException("router reply malformed");
        }
        String[] head = new String(raw, 0, split, StandardCharsets.ISO_8859_1).split("\r\n");
        String[] status = head[0].split(" ");
        if (status.length < 2 || !status[0].startsWith("HTTP/1.")) {
            throw new IOException("router reply malformed");
        }
        int code;
        try {
            code = Integer.parseInt(status[1]);
        } catch (NumberFormatException e) {
            throw new IOException("router reply malformed");
        }
        boolean chunked = false;
        for (String h : head) {
            String lower = h.toLowerCase(Locale.ROOT);
            if (lower.startsWith("transfer-encoding:") && lower.contains("chunked")) {
                chunked = true;
            }
        }
        byte[] body = Arrays.copyOfRange(raw, split + 4, raw.length);
        if (chunked) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            int p = 0;
            while (p < body.length) {
                int eol = p;
                while (eol + 1 < body.length && !(body[eol] == '\r' && body[eol + 1] == '\n')) {
                    eol++;
                }
                String sizeText = new String(body, p, eol - p, StandardCharsets.US_ASCII).trim();
                int semi = sizeText.indexOf(';');
                if (semi >= 0) {
                    sizeText = sizeText.substring(0, semi);
                }
                int size;
                try {
                    size = Integer.parseInt(sizeText, 16);
                } catch (NumberFormatException e) {
                    throw new IOException("router reply malformed");
                }
                p = eol + 2;
                if (size == 0) {
                    break;
                }
                if (p + size > body.length) {
                    throw new IOException("router reply truncated");
                }
                out.write(body, p, size);
                p += size + 2;
            }
            body = out.toByteArray();
        }
        if (body.length > 131072) {
            throw new IOException("router reply too large");
        }
        return new HttpResult(code, new String(body, StandardCharsets.UTF_8));
    }

    // Enough of an XML tree to look things up by local name, like the
    // companion's local-name() XPath. No DTD, no entity beyond the five
    // predefined ones: the kXML pull parser never fetches anything.
    static final class Node {
        final String name;
        final StringBuilder text = new StringBuilder();
        final List<Node> children = new ArrayList<>();

        Node(String name) {
            this.name = name;
        }

        Node find(String local) {
            for (Node c : children) {
                if (c.name.equals(local)) {
                    return c;
                }
                Node deeper = c.find(local);
                if (deeper != null) {
                    return deeper;
                }
            }
            return null;
        }

        void findAll(String local, List<Node> out) {
            for (Node c : children) {
                if (c.name.equals(local)) {
                    out.add(c);
                }
                c.findAll(local, out);
            }
        }

        String value(String local) {
            Node n = find(local);
            return n == null ? null : n.text.toString().trim();
        }
    }

    static Node parseXml(String xml) throws IOException {
        try {
            XmlPullParser p = Xml.newPullParser();
            p.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, true);
            p.setInput(new StringReader(xml));
            Node root = new Node("#document");
            List<Node> stack = new ArrayList<>();
            stack.add(root);
            for (int event = p.getEventType(); event != XmlPullParser.END_DOCUMENT; event = p.next()) {
                if (event == XmlPullParser.START_TAG) {
                    Node n = new Node(p.getName());
                    stack.get(stack.size() - 1).children.add(n);
                    stack.add(n);
                    if (stack.size() > 64) {
                        throw new IOException("XML too deep");
                    }
                } else if (event == XmlPullParser.END_TAG) {
                    stack.remove(stack.size() - 1);
                } else if (event == XmlPullParser.TEXT) {
                    stack.get(stack.size() - 1).text.append(p.getText());
                }
            }
            return root;
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("router XML", e);
        }
    }

    private Node soap(String action, String body) throws IOException {
        HttpResult r = http(control, action, body);
        if (r.status != 200) {
            throw new SoapError(r.status, r.body);
        }
        return parseXml(r.body);
    }

    private static final class SoapError extends IOException {
        final int status;
        final String body;

        SoapError(int status, String body) {
            super("SOAP " + status);
            this.status = status;
            this.body = body;
        }

        String errorCode() {
            try {
                return parseXml(body).value("errorCode");
            } catch (IOException e) {
                return null;
            }
        }
    }

    private URI discover() throws IOException {
        WifiManager.MulticastLock lock = null;
        try {
            WifiManager wifi = context.getApplicationContext().getSystemService(WifiManager.class);
            if (wifi != null) {
                lock = wifi.createMulticastLock("PartyBoard UPnP");
                lock.setReferenceCounted(false);
                lock.acquire();
            }
        } catch (RuntimeException e) {
            lock = null;
        }
        try (MulticastSocket u = new MulticastSocket(new InetSocketAddress(route.local, 0))) {
            u.setInterface(route.local);
            u.setTimeToLive(1);
            u.setSoTimeout(300);
            byte[] q = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
                + "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n").getBytes(StandardCharsets.US_ASCII);
            u.send(new DatagramPacket(q, q.length, InetAddress.getByName("239.255.255.250"), 1900));
            long start = System.nanoTime();
            int packets = 0;
            while ((System.nanoTime() - start) / 1_000_000L < 2000 && packets < 64) {
                byte[] buffer = new byte[8193];
                DatagramPacket reply = new DatagramPacket(buffer, buffer.length);
                try {
                    u.receive(reply);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                packets++;
                if (!reply.getAddress().equals(route.router) || reply.getLength() > 8192) {
                    continue;
                }
                String text = new String(buffer, 0, reply.getLength(), StandardCharsets.US_ASCII);
                if (!text.regionMatches(true, 0, "HTTP/1.1 200", 0, 12)) {
                    continue;
                }
                for (String line : text.split("\n")) {
                    if (line.regionMatches(true, 0, "LOCATION:", 0, 9)) {
                        try {
                            URI uri = new URI(line.substring(9).trim());
                            if (safeUrl(uri)) {
                                return uri;
                            }
                        } catch (Exception ignored) {
                        }
                    }
                }
            }
        } finally {
            if (lock != null) {
                try {
                    lock.release();
                } catch (RuntimeException ignored) {
                }
            }
        }
        throw new IOException("no UPnP answer");
    }

    private String mappingKey() {
        return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + externalPort + "</NewExternalPort><NewProtocol>"
            + (udp ? "UDP" : "TCP") + "</NewProtocol>";
    }

    private void upnp(long life) throws IOException {
        if (control == null) {
            URI url = discover();
            HttpResult description = http(url, null, null);
            if (description.status != 200) {
                throw new IOException("UPnP description");
            }
            List<Node> services = new ArrayList<>();
            parseXml(description.body).findAll("service", services);
            for (Node node : services) {
                String type = node.value("serviceType");
                if (!"urn:schemas-upnp-org:service:WANIPConnection:1".equals(type)
                    && !"urn:schemas-upnp-org:service:WANIPConnection:2".equals(type)
                    && !"urn:schemas-upnp-org:service:WANPPPConnection:1".equals(type))
                {
                    continue;
                }
                String controlUrl = node.value("controlURL");
                if (controlUrl == null) {
                    continue;
                }
                try {
                    URI candidate = url.resolve(controlUrl);
                    if (safeUrl(candidate)) {
                        service = type;
                        control = candidate;
                        break;
                    }
                } catch (IllegalArgumentException ignored) {
                }
            }
            if (control == null) {
                throw new IOException("no WAN service");
            }
            String ip = soap("GetExternalIPAddress", "").value("NewExternalIPAddress");
            InetAddress external = ip == null || !ip.matches("\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}")
                ? null : InetAddress.getByName(ip);
            if (external == null || !isPublic(external)) {
                throw new IOException(Msg.s(
                    "Votre box est derrière un autre réseau. La connexion directe n'est pas disponible ici.",
                    "Your router is behind another network. A direct connection is not available here."));
            }
            address = external;
        }
        if (life == 0) {
            soap("DeletePortMapping", mappingKey());
            return;
        }
        // Never replace a mapping another program owns: only the SOAP
        // NoSuchEntryInArray answer (714) allows creating ours.
        if (!"UPNP".equals(method)) {
            try {
                soap("GetSpecificPortMappingEntry", mappingKey());
                throw new IOException("port already mapped");
            } catch (SoapError e) {
                if (!"714".equals(e.errorCode())) {
                    throw e;
                }
            }
        }
        soap("AddPortMapping", mappingKey() + "<NewInternalPort>" + internalPort + "</NewInternalPort><NewInternalClient>"
            + route.local.getHostAddress() + "</NewInternalClient><NewEnabled>1</NewEnabled>"
            + "<NewPortMappingDescription>PartyBoard temporary session</NewPortMappingDescription><NewLeaseDuration>"
            + life + "</NewLeaseDuration>");
        method = "UPNP"; // cleanup is required even if the router ignores the lease
        Node check = soap("GetSpecificPortMappingEntry", mappingKey());
        long granted;
        try {
            granted = Long.parseLong(check.value("NewLeaseDuration"));
        } catch (RuntimeException e) {
            throw new IOException("mapping not confirmed");
        }
        if (!route.local.getHostAddress().equals(check.value("NewInternalClient"))
            || !String.valueOf(internalPort).equals(check.value("NewInternalPort")))
        {
            throw new IOException("mapping not confirmed");
        }
        lifetime = granted;
        validateLease();
    }

    void open() throws IOException {
        IOException last = null;
        for (String candidate : new String[] { "PCP", "PMP", "UPNP" }) {
            try {
                if (candidate.equals("PCP")) {
                    pcp(120);
                } else if (candidate.equals("PMP")) {
                    pmp(120);
                } else {
                    upnp(120);
                }
                return;
            } catch (IOException | RuntimeException e) {
                last = e instanceof IOException ? (IOException) e : new IOException(e);
                if (method != null) {
                    close();
                }
                method = null;
                address = null;
                externalPort = requestedExternalPort;
            }
        }
        throw new IOException(Msg.s(
            "La box n'a pas autorisé la connexion automatique. Votre ami peut créer le salon à votre place.",
            "The router did not allow the automatic connection. Your friend can create the lobby instead."), last);
    }

    void renew() throws IOException {
        int beforePort = externalPort;
        InetAddress beforeAddress = address;
        if ("PCP".equals(method)) {
            pcp(120);
        } else if ("PMP".equals(method)) {
            pmp(120);
        } else if ("UPNP".equals(method)) {
            upnp(120);
        } else {
            throw new IOException("mapping closed");
        }
        if (beforePort != externalPort || !beforeAddress.equals(address)) {
            throw new IOException(Msg.s("La connexion de la box a changé. Recréez un salon.",
                "The router connection changed. Create the lobby again."));
        }
    }

    @Override
    public void close() {
        try {
            if ("PCP".equals(method)) {
                pcp(0);
            } else if ("PMP".equals(method)) {
                pmp(0);
            } else if ("UPNP".equals(method)) {
                upnp(0);
            }
        } catch (Exception ignored) {
        } finally {
            method = null;
        }
    }
}
