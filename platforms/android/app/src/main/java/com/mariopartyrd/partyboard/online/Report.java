package com.mariopartyrd.partyboard.online;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;
import java.util.UUID;

// tools/online/Report.cs. Structured diagnostics only: never an invitation, an
// address, a nickname, a disc path or free-form engine output.
final class Report {
    private final File directory;
    private int lines;

    private static File root(Context context) {
        return new File(context.getFilesDir(), "online-diagnostics");
    }

    private static String utc(String pattern, Date date) {
        SimpleDateFormat f = new SimpleDateFormat(pattern, Locale.ROOT);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        return f.format(date);
    }

    Report(Context context) {
        File base = root(context);
        directory = new File(base, utc("yyyyMMdd-HHmmss", new Date()) + "-" + UUID.randomUUID().toString().replace("-", ""));
        //noinspection ResultOfMethodCallIgnored
        directory.mkdirs();
        prune(base);
        write("format=1 platform=android");
    }

    private Report(File existing) {
        directory = existing;
    }

    // Keep the ten newest sessions.
    private static void prune(File base) {
        File[] all = base.listFiles(File::isDirectory);
        if (all == null || all.length <= 10) {
            return;
        }
        Arrays.sort(all, (a, b) -> b.getName().compareTo(a.getName()));
        for (int i = 10; i < all.length; i++) {
            File[] files = all[i].listFiles();
            if (files != null) {
                for (File f : files) {
                    //noinspection ResultOfMethodCallIgnored
                    f.delete();
                }
            }
            //noinspection ResultOfMethodCallIgnored
            all[i].delete();
        }
    }

    synchronized void write(String text) {
        if (lines >= 3000) {
            return;
        }
        try (FileOutputStream out = new FileOutputStream(new File(directory, "session.txt"), true)) {
            out.write((utc("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", new Date()) + " " + text + "\n").getBytes(StandardCharsets.UTF_8));
            lines++;
        } catch (IOException ignored) {
        }
    }

    private static void section(StringBuilder result, String title, File file, int budget) {
        result.append("--- ").append(title).append(" ---\n");
        if (file == null || !file.isFile()) {
            result.append("No data available.\n");
            return;
        }
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            long length = f.length();
            int n = (int) Math.min(length, budget);
            byte[] bytes = new byte[n];
            f.readFully(bytes);
            result.append(new String(bytes, StandardCharsets.UTF_8));
            if (length > budget) {
                result.append("\n[fin tronquee]\n");
            }
        } catch (IOException e) {
            result.append("No data available.\n");
        }
    }

    String read() {
        StringBuilder result = new StringBuilder("PartyBoard diagnostic v1 (android)\n");
        section(result, "session.txt", new File(directory, "session.txt"), 1024 * 1024);
        return result.toString();
    }

    static String readLatest(Context context) throws IOException {
        File[] all = root(context).listFiles(File::isDirectory);
        if (all == null || all.length == 0) {
            throw new IOException(Msg.s("Créez ou rejoignez un salon avant d'exporter le diagnostic.",
                "Create or join a lobby before exporting the diagnostic."));
        }
        Arrays.sort(all, (a, b) -> b.getName().compareTo(a.getName()));
        return new Report(all[0]).read();
    }
}
