package com.mariopartyrd.partyboard;

import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.zip.GZIPInputStream;
import java.util.zip.GZIPOutputStream;

import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * The phone's side of CubeShelf's QR-code transfer (CubeShelf.Core/Social/PhoneLink.cs).
 *
 * The code reads "CSL1:192.168.1.20:48213/token#key": the PC's address on this Wi-Fi, a request
 * token and a 256-bit AES-GCM key. Every body is sealed under that key, so plain HTTP on the local
 * network is fine: nobody who has not seen the PC's screen can read or change a byte.
 *
 * Saves received from the PC are not written over the running game's memory cards: they wait in
 * files/cubeshelf-incoming and {@link #applyIncoming} puts them in place at the next start, before
 * the game opens its cards, keeping what they replace in files/save-backups.
 */
final class CubeShelfLink {
    static final String PREFIX = "CSL1:";
    private static final int NONCE = 12;
    private static final int TAG = 16;
    private static final int MAX_BODY = 128 * 1024 * 1024;
    static final String INCOMING = "cubeshelf-incoming";
    static final String BACKUPS = "save-backups";

    final InetAddress host;
    final int port;
    final String token;
    final byte[] key;

    private CubeShelfLink(InetAddress host, int port, String token, byte[] key) {
        this.host = host;
        this.port = port;
        this.token = token;
        this.key = key;
    }

    static final class LinkException extends IOException {
        LinkException(String message) {
            super(message);
        }
    }

    static boolean isPrivate(InetAddress address) {
        if (!(address instanceof Inet4Address)) {
            return false;
        }
        byte[] b = address.getAddress();
        int b0 = b[0] & 0xff, b1 = b[1] & 0xff;
        return b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168);
    }

    static CubeShelfLink parse(String text, boolean french) throws LinkException {
        String code = text == null ? "" : text.trim();
        int at = code.indexOf(PREFIX);
        if (at < 0) {
            throw new LinkException(french ? "Ce n'est pas un QR code de CubeShelf." : "This is not a CubeShelf QR code.");
        }
        code = code.substring(at + PREFIX.length()).split("\\s")[0];
        try {
            int slash = code.indexOf('/');
            int hash = code.indexOf('#');
            int colon = code.lastIndexOf(':', slash);
            String ip = code.substring(0, colon);
            int port = Integer.parseInt(code.substring(colon + 1, slash));
            String token = code.substring(slash + 1, hash);
            byte[] key = Base64.decode(code.substring(hash + 1), Base64.URL_SAFE | Base64.NO_PADDING | Base64.NO_WRAP);
            if (!ip.matches("\\d{1,3}(\\.\\d{1,3}){3}") || port <= 0 || port > 65535 || key.length != 32
                || !token.matches("[A-Za-z0-9_-]{16,64}"))
            {
                throw new IllegalArgumentException();
            }
            InetAddress host = InetAddress.getByName(ip);
            // The PC is on this network, never somewhere a code could send the phone to.
            if (!isPrivate(host)) {
                throw new IllegalArgumentException();
            }
            return new CubeShelfLink(host, port, token, key);
        } catch (RuntimeException | IOException e) {
            throw new LinkException(french ? "QR code CubeShelf incomplet." : "Incomplete CubeShelf QR code.");
        }
    }

    // ---- Sealing (PhoneLink.Seal / Open) ------------------------------------------------------

    private byte[] aad(String direction) {
        return ("cubeshelf-link-v1|" + direction).getBytes(StandardCharsets.US_ASCII);
    }

    byte[] seal(String direction, byte[] plaintext) throws GeneralSecurityException {
        byte[] nonce = new byte[NONCE];
        new SecureRandom().nextBytes(nonce);
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(TAG * 8, nonce));
        cipher.updateAAD(aad(direction));
        byte[] sealed = cipher.doFinal(plaintext); // ciphertext || tag, as AesGcm lays them out
        byte[] framed = new byte[NONCE + sealed.length];
        System.arraycopy(nonce, 0, framed, 0, NONCE);
        System.arraycopy(sealed, 0, framed, NONCE, sealed.length);
        return framed;
    }

    byte[] open(String direction, byte[] framed) throws GeneralSecurityException {
        if (framed.length < NONCE + TAG) {
            throw new GeneralSecurityException("short");
        }
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"),
            new GCMParameterSpec(TAG * 8, Arrays.copyOf(framed, NONCE)));
        cipher.updateAAD(aad(direction));
        return cipher.doFinal(framed, NONCE, framed.length - NONCE);
    }

    // ---- HTTP (the PC answers exactly two requests) --------------------------------------------

    private byte[] request(String method, String path, byte[] body, boolean french) throws IOException {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(host, port), 5000);
            socket.setSoTimeout(60000);
            OutputStream out = socket.getOutputStream();
            String head = method + " " + path + " HTTP/1.1\r\nHost: " + host.getHostAddress() + ":" + port
                + "\r\nContent-Length: " + (body == null ? 0 : body.length) + "\r\nConnection: close\r\n\r\n";
            out.write(head.getBytes(StandardCharsets.US_ASCII));
            if (body != null) {
                out.write(body);
            }
            out.flush();
            socket.shutdownOutput(); // lets the PC see the request is complete and close cleanly
            InputStream in = socket.getInputStream();
            ByteArrayOutputStream raw = new ByteArrayOutputStream();
            byte[] buffer = new byte[65536];
            int n;
            while ((n = in.read(buffer)) > 0) {
                raw.write(buffer, 0, n);
                if (raw.size() > MAX_BODY + 8192) {
                    throw new LinkException("too large");
                }
            }
            byte[] all = raw.toByteArray();
            int split = -1;
            for (int i = 0; i + 3 < all.length; i++) {
                if (all[i] == '\r' && all[i + 1] == '\n' && all[i + 2] == '\r' && all[i + 3] == '\n') {
                    split = i;
                    break;
                }
            }
            if (split < 0) {
                throw new LinkException("malformed reply");
            }
            String[] replyHead = new String(all, 0, split, StandardCharsets.ISO_8859_1).split("\r\n");
            String status = replyHead[0];
            long expected = -1;
            for (String line : replyHead) {
                if (line.regionMatches(true, 0, "Content-Length:", 0, 15)) {
                    expected = Long.parseLong(line.substring(15).trim());
                }
            }
            if (expected >= 0 && all.length - split - 4 != expected) {
                throw new LinkException(french ? "Transfert incomplet, réessaie." : "Incomplete transfer, try again.");
            }
            if (!status.contains(" 200 ")) {
                throw new LinkException(french
                    ? "Le PC a refusé la demande : affiche un nouveau QR code dans CubeShelf."
                    : "The PC refused the request: show a new QR code in CubeShelf.");
            }
            return Arrays.copyOfRange(all, split + 4, all.length);
        } catch (java.net.ConnectException | java.net.SocketTimeoutException | java.net.NoRouteToHostException e) {
            throw new LinkException(french
                ? "PC injoignable : le téléphone doit être sur le même Wi-Fi que le PC, et le QR code encore affiché."
                : "PC unreachable: the phone has to be on the same Wi-Fi as the PC, with the QR code still shown.");
        }
    }

    // ---- The two directions --------------------------------------------------------------------

    static final class Received {
        String profileJson;
        int saves;
    }

    /** Fetches the profile and the PC's saves; the saves wait in files/cubeshelf-incoming. */
    Received fetch(File filesDir, boolean french) throws IOException {
        byte[] body = request("GET", "/l/" + token, null, french);
        byte[] plaintext;
        try {
            plaintext = open("down", body);
        } catch (GeneralSecurityException e) {
            android.util.Log.w("PartyBoard", "CubeShelf reply of " + body.length + " bytes not opened", e);
            throw new LinkException(french ? "Réponse du PC illisible." : "Unreadable reply from the PC.");
        }
        try {
            JSONObject document = new JSONObject(new String(plaintext, StandardCharsets.UTF_8));
            if (document.optInt("v") != 1) {
                throw new LinkException(french ? "Version de CubeShelf non prise en charge." : "Unsupported CubeShelf version.");
            }
            Received received = new Received();
            JSONObject profile = document.optJSONObject("profile");
            received.profileJson = profile != null ? profile.toString() : null;
            JSONArray saves = document.optJSONArray("saves");
            File incoming = new File(filesDir, INCOMING);
            deleteTree(incoming);
            for (int i = 0; saves != null && i < saves.length(); i++) {
                JSONObject save = saves.getJSONObject(i);
                String path = save.getString("path");
                if (!isSavePath(path)) {
                    continue;
                }
                byte[] data = gunzip(Base64.decode(save.getString("gz"), Base64.DEFAULT));
                File target = new File(incoming, path);
                //noinspection ResultOfMethodCallIgnored
                target.getParentFile().mkdirs();
                try (FileOutputStream out = new FileOutputStream(target)) {
                    out.write(data);
                }
                received.saves++;
            }
            return received;
        } catch (JSONException | IllegalArgumentException e) {
            throw new LinkException(french ? "Données du PC illisibles." : "Unreadable data from the PC.");
        }
    }

    /** Sends this phone's saves to the PC and returns what the PC says it did. */
    String send(File filesDir, boolean french) throws IOException {
        try {
            JSONArray saves = new JSONArray();
            SimpleDateFormat iso = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'+00:00'", Locale.ROOT);
            iso.setTimeZone(TimeZone.getTimeZone("UTC"));
            for (String path : listSaves(filesDir)) {
                File file = new File(filesDir, path);
                JSONObject save = new JSONObject();
                save.put("path", path);
                save.put("modified", iso.format(new Date(file.lastModified())));
                save.put("gz", Base64.encodeToString(gzip(readAll(file)), Base64.NO_WRAP));
                saves.put(save);
            }
            JSONObject document = new JSONObject();
            document.put("v", 1);
            document.put("saves", saves);
            byte[] body = request("POST", "/l/" + token + "/saves",
                seal("up", document.toString().getBytes(StandardCharsets.UTF_8)), french);
            return new String(open("down", body), StandardCharsets.UTF_8);
        } catch (JSONException | GeneralSecurityException e) {
            throw new LinkException(french ? "Envoi impossible." : "Could not send.");
        }
    }

    // ---- Saves on the phone ----------------------------------------------------------------------

    /** Same rule as PhoneLink.IsSavePath: memory cards only, never a path outside the folder. */
    static boolean isSavePath(String relative) {
        String[] parts = relative.replace('\\', '/').split("/", -1);
        for (String part : parts) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) {
                return false;
            }
        }
        if (parts.length == 1) {
            return parts[0].startsWith("MemoryCard") && parts[0].toLowerCase(Locale.ROOT).endsWith(".raw");
        }
        return parts.length == 3 && (parts[0].equals("USA") || parts[0].equals("EUR") || parts[0].equals("JAP"))
            && (parts[1].equals("Card A") || parts[1].equals("Card B"))
            && parts[2].toLowerCase(Locale.ROOT).endsWith(".gci");
    }

    static List<String> listSaves(File root) {
        List<String> out = new ArrayList<>();
        collect(root, root, out);
        return out;
    }

    private static void collect(File root, File dir, List<String> out) {
        File[] files = dir.listFiles();
        if (files == null) {
            return;
        }
        for (File f : files) {
            String relative = root.toURI().relativize(f.toURI()).getPath();
            if (f.isDirectory()) {
                if (!f.getName().equals(INCOMING) && !f.getName().equals(BACKUPS) && relative.split("/").length < 3) {
                    collect(root, f, out);
                }
            } else if (isSavePath(relative)) {
                out.add(relative);
            }
        }
    }

    /**
     * Puts received saves in place, before the game opens its memory cards. What they replace goes
     * to files/save-backups/{time}; the five newest backups are kept.
     */
    static int applyIncoming(File filesDir) {
        File incoming = new File(filesDir, INCOMING);
        List<String> paths = listSaves(incoming);
        if (paths.isEmpty()) {
            deleteTree(incoming);
            return 0;
        }
        SimpleDateFormat stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT);
        File backup = new File(new File(filesDir, BACKUPS), stamp.format(new Date()));
        int applied = 0;
        for (String path : paths) {
            File source = new File(incoming, path);
            File target = new File(filesDir, path);
            try {
                if (target.exists()) {
                    File copy = new File(backup, path);
                    //noinspection ResultOfMethodCallIgnored
                    copy.getParentFile().mkdirs();
                    try (FileOutputStream out = new FileOutputStream(copy)) {
                        out.write(readAll(target));
                    }
                }
                //noinspection ResultOfMethodCallIgnored
                target.getParentFile().mkdirs();
                if (!source.renameTo(target)) {
                    try (FileOutputStream out = new FileOutputStream(target)) {
                        out.write(readAll(source));
                    }
                }
                applied++;
            } catch (IOException e) {
                android.util.Log.w("PartyBoard", "Could not put a received save in place: " + path, e);
            }
        }
        deleteTree(incoming);
        File[] backups = new File(filesDir, BACKUPS).listFiles(File::isDirectory);
        if (backups != null && backups.length > 5) {
            Arrays.sort(backups, (a, b) -> b.getName().compareTo(a.getName()));
            for (int i = 5; i < backups.length; i++) {
                deleteTree(backups[i]);
            }
        }
        return applied;
    }

    static byte[] readAll(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream((int) Math.min(file.length(), MAX_BODY));
            byte[] buffer = new byte[65536];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        }
    }

    static byte[] gzip(byte[] data) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (GZIPOutputStream gz = new GZIPOutputStream(out)) {
            gz.write(data);
        }
        return out.toByteArray();
    }

    static byte[] gunzip(byte[] data) throws IOException {
        try (GZIPInputStream in = new GZIPInputStream(new java.io.ByteArrayInputStream(data))) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[65536];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
                if (out.size() > MAX_BODY) {
                    throw new LinkException("save too large");
                }
            }
            return out.toByteArray();
        }
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
