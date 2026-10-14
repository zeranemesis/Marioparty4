package com.mariopartyrd.partyboard.online;

import java.util.Locale;

// The lobby speaks the language the game menus use (Settings > Language), which
// the game hands over when it opens the lobby. French and English, like the
// rest of Party Board; every message is written next to its translation so the
// two cannot drift apart the way separate resource files quietly do.
final class Msg {
    private static volatile boolean french = Locale.getDefault().getLanguage().equals("fr");
    // The game's own language, one of GAME_LANGUAGES: a European disc carries
    // five, and the host's is the one every seat loads.
    private static volatile String gameLanguage = "en";
    // Indexed as the game numbers them (OSGetLanguage).
    static final String[] GAME_LANGUAGES = { "en", "de", "fr", "es", "it" };

    private Msg() {}

    static void setLanguage(String code) {
        if (code != null && !code.isEmpty()) {
            french = code.equals("fr");
            if (gameLanguageIndex(code) >= 0) {
                gameLanguage = code;
            }
        }
    }

    static String gameLanguage() {
        return gameLanguage;
    }

    // -1 for a code the game does not know.
    static int gameLanguageIndex(String code) {
        for (int i = 0; i < GAME_LANGUAGES.length; i++) {
            if (GAME_LANGUAGES[i].equals(code)) {
                return i;
            }
        }
        return -1;
    }

    static boolean french() {
        return french;
    }

    static String s(String fr, String en) {
        return french ? fr : en;
    }
}
