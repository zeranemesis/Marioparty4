package com.mariopartyrd.partyboard.online;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.function.BooleanSupplier;
import java.util.function.IntConsumer;

// tools/online/Lobby.cs (DiscFile). The whole file is hashed: two players only
// play together when they have exactly the same bytes, which is what keeps
// both games in lockstep.
final class DiscFile {
    final String path;
    final String displayName;
    final byte[] hash;
    final long length;

    private DiscFile(String path, String displayName, byte[] hash, long length) {
        this.path = path;
        this.displayName = displayName;
        this.hash = hash;
        this.length = length;
    }

    static InputStream open(Context context, String path) throws IOException {
        try {
            if (path.startsWith("content:") || path.startsWith("file:")) {
                InputStream in = context.getContentResolver().openInputStream(Uri.parse(path));
                if (in == null) {
                    throw new IOException("unreadable");
                }
                return in;
            }
            return new FileInputStream(path);
        } catch (IOException | SecurityException e) {
            // A moved or deleted file, or a permission Android took back.
            throw new IOException(Msg.s("Ce fichier disque ne peut plus être lu. Choisissez-le à nouveau.",
                "This disc file can no longer be read. Choose it again."), e);
        }
    }

    static String displayName(Context context, String path) {
        if (path.startsWith("content:")) {
            try (Cursor c = context.getContentResolver().query(Uri.parse(path),
                new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null))
            {
                if (c != null && c.moveToFirst() && !c.isNull(0)) {
                    return c.getString(0);
                }
            } catch (RuntimeException ignored) {
            }
        }
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? Uri.decode(path.substring(slash + 1)) : path;
    }

    private static IOException wrongDisc() {
        return new IOException(Msg.s(
            "Choisissez un disque Mario Party 4 USA, révision 1 (ISO, GCM, RVZ, WIA ou CISO). Ce fichier n'est pas compatible.",
            "Choose a Mario Party 4 USA revision 1 disc (ISO, GCM, RVZ, WIA or CISO). This file is not compatible."));
    }

    private static int u32be(byte[] b, int p) {
        return ((b[p] & 0xff) << 24) | ((b[p + 1] & 0xff) << 16) | ((b[p + 2] & 0xff) << 8) | (b[p + 3] & 0xff);
    }

    // The PC checks the header through nod before hashing (partyboard.exe
    // --online-disc-check): GMPE01, disc version 1. Every container a phone is
    // likely to hold keeps that header uncompressed at a known offset.
    static void checkHeader(byte[] head, int n) throws IOException {
        int at;
        if (n >= 0x60 && (head[0] == 'R' || head[0] == 'W') && head[1] == (head[0] == 'R' ? 'V' : 'I')
            && head[2] == (head[0] == 'R' ? 'Z' : 'A') && head[3] == 1)
        {
            at = 0x58; // RVZ/WIA: wia_file_head_2.dhead, right after the 0x48-byte file head
        } else if (n >= 8 && head[0] == 'C' && head[1] == 'I' && head[2] == 'S' && head[3] == 'O') {
            at = 0x8000; // CISO: the first block follows the 0x8000-byte map
        } else if (n >= 0x20 && u32be(head, 0x1c) == 0xC2339F3D) {
            at = 0; // plain GameCube image (ISO/GCM): disc magic at 0x1C
        } else {
            throw wrongDisc();
        }
        if (n < at + 8) {
            throw wrongDisc();
        }
        String id = new String(head, at, 6, StandardCharsets.US_ASCII);
        if (!id.equals("GMPE01") || head[at + 7] != 1) {
            throw wrongDisc();
        }
    }

    static DiscFile verify(Context context, String path, IntConsumer progress, BooleanSupplier cancelled)
        throws IOException
    {
        long length = -1;
        try (Cursor c = path.startsWith("content:") ? context.getContentResolver().query(Uri.parse(path),
            new String[] { OpenableColumns.SIZE }, null, null, null) : null)
        {
            if (c != null && c.moveToFirst() && !c.isNull(0)) {
                length = c.getLong(0);
            }
        } catch (RuntimeException ignored) {
        }
        MessageDigest sha = Wire.sha256();
        byte[] buffer = new byte[1 << 20];
        long read = 0;
        int last = -1;
        try (InputStream in = open(context, path)) {
            int headSize = 0;
            byte[] head = new byte[0x8010];
            while (headSize < head.length) {
                int k = in.read(head, headSize, head.length - headSize);
                if (k < 0) {
                    break;
                }
                headSize += k;
            }
            if (headSize == 0) {
                throw new IOException(Msg.s("Ce fichier disque est vide.", "This disc file is empty."));
            }
            checkHeader(head, headSize);
            sha.update(head, 0, headSize);
            read = headSize;
            int n;
            while ((n = in.read(buffer)) > 0) {
                if (cancelled.getAsBoolean()) {
                    throw new IOException(Msg.s("Vérification annulée.", "Verification cancelled."));
                }
                sha.update(buffer, 0, n);
                read += n;
                if (length > 0) {
                    int pct = (int) Math.min(100, read * 100 / length);
                    if (pct != last) {
                        last = pct;
                        progress.accept(pct);
                    }
                }
            }
        }
        if (length > 0 && read != length) {
            throw new IOException(Msg.s("Le disque n'a pas pu être lu entièrement.", "The disc could not be read completely."));
        }
        return new DiscFile(path, displayName(context, path), sha.digest(), read);
    }
}
