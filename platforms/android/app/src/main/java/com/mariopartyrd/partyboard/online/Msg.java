package com.mariopartyrd.partyboard.online;

import java.util.Locale;

// The lobby speaks the language the game menus use (Settings > Language), which
// the game hands over when it opens the lobby. French and English, like the
// rest of Party Board; every message is written next to its translation so the
// two cannot drift apart the way separate resource files quietly do.
final class Msg {
    private static volatile boolean french = Locale.getDefault().getLanguage().equals("fr");

    private Msg() {}

    static void setLanguage(String code) {
        if (code != null && !code.isEmpty()) {
            french = code.equals("fr");
        }
    }

    static boolean french() {
        return french;
    }

    static String s(String fr, String en) {
        return french ? fr : en;
    }
}
