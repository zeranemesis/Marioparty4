package com.mariopartyrd.partyboard.online;

import android.content.Context;
import android.content.pm.ApplicationInfo;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.cert.Certificate;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

// tools/online/Connection.cs (Wire): hashes, the session certificate and the
// PBAUTO3 handshake both channels of a guest go through.
final class Wire {
    static final SecureRandom RANDOM = new SecureRandom();
    // The native game speaks IPv4 loopback; InetAddress.getLoopbackAddress()
    // is ::1 on Android, which the game never reaches.
    static final InetAddress LOOPBACK;

    static {
        try {
            LOOPBACK = InetAddress.getByAddress("localhost", new byte[] { 127, 0, 0, 1 });
        } catch (java.net.UnknownHostException e) {
            throw new IllegalStateException(e);
        }
    }
    private static final byte[] HELLO = "PBAUTO3\n".getBytes(StandardCharsets.US_ASCII);
    static final int HELLO_SIZE = 89;

    private Wire() {}

    // The host answered, and said no: a different build or a dead invitation.
    // Unlike a network failure, trying another address will not change it.
    static final class Refused extends IOException {
        Refused(String message) {
            super(message);
        }
    }

    static byte[] random(int n) {
        byte[] b = new byte[n];
        RANDOM.nextBytes(b);
        return b;
    }

    static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    static byte[] hash(byte[]... parts) {
        MessageDigest h = sha256();
        for (byte[] part : parts) {
            h.update(part);
        }
        return h.digest();
    }

    // Constant time, and false for any null or length mismatch.
    static boolean equal(byte[] a, byte[] b) {
        if (a == null || b == null || a.length != b.length) {
            return false;
        }
        int d = 0;
        for (int i = 0; i < a.length; i++) {
            d |= a[i] ^ b[i];
        }
        return d == 0;
    }

    static byte[] readFully(InputStream in, int n) throws IOException {
        byte[] b = new byte[n];
        int p = 0;
        while (p < n) {
            int k = in.read(b, p, n - p);
            if (k < 0) {
                throw new IOException(Msg.s("L'autre joueur s'est déconnecté.", "The other player disconnected."));
            }
            p += k;
        }
        return b;
    }

    static String hex(byte[] b) {
        StringBuilder s = new StringBuilder(b.length * 2);
        for (byte v : b) {
            s.append(String.format(Locale.ROOT, "%02x", v & 0xff));
        }
        return s.toString();
    }

    // On a PC the build is every DLL plus the two executables. Here it is the
    // installed APK itself (and any split), which is exactly what two phones
    // have to share byte for byte: same game code, same native library, same
    // resources. The "android" prefix keeps it from ever matching a PC build,
    // whose floating point runs on another architecture and would desync.
    static byte[] buildHash(Context context) throws IOException {
        ApplicationInfo info = context.getApplicationInfo();
        ByteArrayOutputStream list = new ByteArrayOutputStream();
        list.write("partyboard-android\n".getBytes(StandardCharsets.US_ASCII));
        String[] splits = info.splitSourceDirs == null ? new String[0] : info.splitSourceDirs.clone();
        Arrays.sort(splits);
        String[] paths = new String[splits.length + 1];
        paths[0] = info.sourceDir;
        System.arraycopy(splits, 0, paths, 1, splits.length);
        byte[] buffer = new byte[1 << 20];
        for (String path : paths) {
            MessageDigest h = sha256();
            try (InputStream in = new FileInputStream(path)) {
                int n;
                while ((n = in.read(buffer)) > 0) {
                    h.update(buffer, 0, n);
                }
            }
            String name = path.substring(path.lastIndexOf('/') + 1).toLowerCase(Locale.ROOT) + "\n";
            list.write(name.getBytes(StandardCharsets.UTF_8));
            list.write(h.digest());
        }
        return hash(list.toByteArray());
    }

    // A self-signed certificate that lives only as long as the lobby. Nothing
    // trusts it through a chain: the guest pins the first 16 bytes of its
    // SHA-256, carried by the invitation, which is all the identity it needs.
    static final class Identity {
        final KeyPair keys;
        final X509Certificate certificate;
        final byte[] fingerprint;
        final SSLContext server;

        Identity(KeyPair keys, X509Certificate certificate) throws IOException {
            this.keys = keys;
            this.certificate = certificate;
            try {
                fingerprint = Arrays.copyOf(hash(certificate.getEncoded()), 16);
                char[] password = new char[0];
                KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
                store.load(null, null);
                store.setKeyEntry("session", keys.getPrivate(), password, new Certificate[] { certificate });
                KeyManagerFactory kmf = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
                kmf.init(store, password);
                server = SSLContext.getInstance("TLS");
                server.init(kmf.getKeyManagers(), null, RANDOM);
            } catch (Exception e) {
                throw new IOException(Msg.s("Impossible de préparer le salon chiffré.", "Unable to prepare the encrypted lobby."), e);
            }
        }
    }

    static Identity certificate() throws IOException {
        try {
            KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA");
            generator.initialize(2048, RANDOM);
            KeyPair keys = generator.generateKeyPair();
            long now = System.currentTimeMillis();
            byte[] tbs = Der.tbsCertificate(keys.getPublic().getEncoded(), new Date(now - 5 * 60_000L),
                new Date(now + 12 * 3_600_000L));
            Signature signer = Signature.getInstance("SHA256withRSA");
            signer.initSign(keys.getPrivate());
            signer.update(tbs);
            byte[] der = Der.certificate(tbs, signer.sign());
            X509Certificate cert = (X509Certificate) CertificateFactory.getInstance("X.509")
                .generateCertificate(new ByteArrayInputStream(der));
            return new Identity(keys, cert);
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException(Msg.s("Impossible de préparer le salon chiffré.", "Unable to prepare the encrypted lobby."), e);
        }
    }

    private static final class PinnedTrust implements X509TrustManager {
        private final byte[] fingerprint;

        PinnedTrust(byte[] fingerprint) {
            this.fingerprint = fingerprint;
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
            throw new CertificateException("client certificates are not used");
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
            if (chain == null || chain.length == 0
                || !equal(Arrays.copyOf(hash(chain[0].getEncoded()), fingerprint.length), fingerprint)) {
                throw new CertificateException("certificate does not match the invitation");
            }
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
            return new X509Certificate[0];
        }
    }

    private static void configure(SSLSocket ssl) {
        ssl.setEnabledProtocols(new String[] { "TLSv1.2" });
    }

    // One guest opens two channels, and with three guests the host receives
    // six connections in any order: the client id says whose each one is.
    static SSLSocket client(Socket tcp, Invitation invite, byte[] build, int channel, byte[] clientId)
        throws IOException
    {
        if (clientId == null) {
            clientId = new byte[16];
        }
        if (clientId.length != 16) {
            throw new IOException(Msg.s("Identifiant de connexion incorrect.", "Invalid connection identifier."));
        }
        tcp.setTcpNoDelay(true);
        tcp.setSoTimeout(8000);
        SSLSocket ssl;
        try {
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, new TrustManager[] { new PinnedTrust(invite.fingerprint) }, RANDOM);
            ssl = (SSLSocket) context.getSocketFactory().createSocket(tcp,
                tcp.getInetAddress().getHostAddress(), tcp.getPort(), true);
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException(e);
        }
        try {
            configure(ssl);
            ssl.setUseClientMode(true);
            ssl.startHandshake();
            ByteArrayOutputStream hello = new ByteArrayOutputStream(HELLO_SIZE);
            hello.write(HELLO);
            hello.write(hash(invite.token));
            hello.write(build);
            hello.write(channel);
            hello.write(clientId);
            OutputStream out = ssl.getOutputStream();
            out.write(hello.toByteArray());
            out.flush();
            int ack = readFully(ssl.getInputStream(), 1)[0];
            if (ack == 2) {
                throw new Refused(Msg.s(
                    "Les versions du jeu sont différentes. Installez la même version de Party Board sur tous les téléphones (un téléphone ne joue qu'avec des téléphones, pas avec un PC).",
                    "The game versions differ. Install the same Party Board build on every phone (phones play with phones, not with a PC)."));
            }
            if (ack != 1) {
                throw new Refused(Msg.s("Cette invitation n'est plus valable. Demandez-en une nouvelle.",
                    "This invitation is no longer valid. Ask for a new one."));
            }
            ssl.setSoTimeout(120000);
            return ssl;
        } catch (IOException e) {
            closeQuietly(ssl);
            throw e;
        }
    }

    static final class Accepted {
        final SSLSocket ssl;
        final int channel;
        final byte[] clientId;

        Accepted(SSLSocket ssl, int channel, byte[] clientId) {
            this.ssl = ssl;
            this.channel = channel;
            this.clientId = clientId;
        }
    }

    static Accepted server(Socket tcp, Identity identity, Invitation invite, byte[] build) throws IOException {
        tcp.setTcpNoDelay(true);
        tcp.setSoTimeout(8000);
        SSLSocket ssl = (SSLSocket) identity.server.getSocketFactory().createSocket(tcp,
            tcp.getInetAddress().getHostAddress(), tcp.getPort(), true);
        try {
            configure(ssl);
            ssl.setUseClientMode(false);
            ssl.startHandshake();
            byte[] hello = readFully(ssl.getInputStream(), HELLO_SIZE);
            int channel = hello[72] & 0xff;
            byte[] clientId = Arrays.copyOfRange(hello, 73, 89);
            if (!equal(Arrays.copyOf(hello, 8), HELLO) || channel > 1
                || !equal(Arrays.copyOfRange(hello, 8, 40), hash(invite.token))
                || System.currentTimeMillis() > invite.expiresMillis())
            {
                throw new IOException("Invitation refused.");
            }
            OutputStream out = ssl.getOutputStream();
            if (!equal(Arrays.copyOfRange(hello, 40, 72), build)) {
                out.write(2);
                out.flush();
                throw new IOException("Different builds.");
            }
            out.write(1);
            out.flush();
            ssl.setSoTimeout(120000);
            return new Accepted(ssl, channel, clientId);
        } catch (IOException e) {
            closeQuietly(ssl);
            throw e;
        }
    }

    static void closeQuietly(AutoCloseable c) {
        if (c != null) {
            try {
                c.close();
            } catch (Exception ignored) {
            }
        }
    }

    // Just enough DER for one self-signed RSA certificate.
    static final class Der {
        private static final byte[] SHA256_WITH_RSA = {
            0x30, 0x0d, 0x06, 0x09, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00
        };

        private Der() {}

        static byte[] tlv(int tag, byte[]... contents) {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            for (byte[] c : contents) {
                body.write(c, 0, c.length);
            }
            byte[] value = body.toByteArray();
            ByteArrayOutputStream out = new ByteArrayOutputStream(value.length + 6);
            out.write(tag);
            int n = value.length;
            if (n < 0x80) {
                out.write(n);
            } else if (n < 0x100) {
                out.write(0x81);
                out.write(n);
            } else if (n < 0x10000) {
                out.write(0x82);
                out.write(n >> 8);
                out.write(n);
            } else {
                out.write(0x83);
                out.write(n >> 16);
                out.write(n >> 8);
                out.write(n);
            }
            out.write(value, 0, value.length);
            return out.toByteArray();
        }

        static byte[] name() {
            byte[] cn = { 0x06, 0x03, 0x55, 0x04, 0x03 };
            byte[] value = tlv(0x0c, "PartyBoard session".getBytes(StandardCharsets.UTF_8));
            return tlv(0x30, tlv(0x31, tlv(0x30, cn, value)));
        }

        static byte[] time(Date date) {
            SimpleDateFormat format = new SimpleDateFormat("yyMMddHHmmss'Z'", Locale.ROOT);
            format.setTimeZone(TimeZone.getTimeZone("UTC"));
            return tlv(0x17, format.format(date).getBytes(StandardCharsets.US_ASCII));
        }

        static byte[] tbsCertificate(byte[] subjectPublicKeyInfo, Date notBefore, Date notAfter) {
            byte[] serial = random(16);
            serial[0] = (byte) ((serial[0] & 0x7f) | 0x40); // positive, no leading zero
            byte[] version = tlv(0xa0, tlv(0x02, new byte[] { 2 }));
            return tlv(0x30, version, tlv(0x02, serial), SHA256_WITH_RSA, name(),
                tlv(0x30, time(notBefore), time(notAfter)), name(), subjectPublicKeyInfo);
        }

        static byte[] certificate(byte[] tbs, byte[] signature) {
            byte[] bits = new byte[signature.length + 1];
            System.arraycopy(signature, 0, bits, 1, signature.length);
            return tlv(0x30, tbs, SHA256_WITH_RSA, tlv(0x03, bits));
        }
    }
}
