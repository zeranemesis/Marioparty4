// Host unit tests for the launcher's portable core: disc parsing in every
// supported container, library classification, settings, launch arguments,
// the boot timeline and the synthesised sounds.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "boot.hpp"
#include "catalog.hpp"
#include "demo_library.hpp"
#include "disc.hpp"
#include "image.hpp"
#include "json.hpp"
#include "library.hpp"
#include "mods.hpp"
#include "paths.hpp"
#include "settings.hpp"
#include "sound.hpp"

using namespace partyboard::launcher;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(condition)) {                                                              \
            ++g_failures;                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

std::string tempDir() {
    char pattern[] = "/tmp/partyboard-launcher-test-XXXXXX";
    const char* dir = mkdtemp(pattern);
    return dir ? dir : "/tmp";
}

void testTextConversion() {
    const char latin[] = "Jusqu'\xE0 quatre \x80 \x85";
    CHECK_EQ(discTextToUtf8(latin, sizeof(latin), false), "Jusqu'\xC3\xA0 quatre \xE2\x82\xAC \xE2\x80\xA6");
    const char sjis[] = "\x83\x7D\x83\x8A MP4 \xB1";
    CHECK_EQ(discTextToUtf8(sjis, sizeof(sjis), true), "MP4");
    const char padded[16] = "Title   ";
    CHECK_EQ(discTextToUtf8(padded, sizeof(padded), false), "Title");
}

void testRgb5a3() {
    // One 4x4 tile: opaque pure red, then a translucent grey texel.
    uint8_t tile[32] = {};
    tile[0] = 0xFC; // 1 11111 00000 00000
    tile[1] = 0x00;
    tile[2] = 0x48; // 0 100 1000 1000 1000: alpha 4/7, grey 0x88
    tile[3] = 0x88;
    uint8_t rgba[4 * 4 * 4];
    decodeRgb5a3(tile, 4, 4, rgba);
    CHECK_EQ(rgba[0], 255);
    CHECK_EQ(rgba[1], 0);
    CHECK_EQ(rgba[2], 0);
    CHECK_EQ(rgba[3], 255);
    CHECK_EQ(rgba[4], 0x88);
    CHECK_EQ(rgba[5], 0x88);
    CHECK_EQ(rgba[6], 0x88);
    CHECK_EQ(rgba[7], (4 << 5) | (4 << 2) | (4 >> 1));
}

void expectDemoBanner(const DiscInfo& info, int art) {
    CHECK(info.hasBanner());
    if (!info.hasBanner())
        return;
    // RGB555 quantisation of the source art: within one 5-bit step.
    const std::vector<uint8_t> source = demo::bannerArt(art);
    int worst = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        if (i % 4 == 3)
            continue;
        worst = std::max(worst, std::abs(int(source[i]) - int(info.bannerRgba[i])));
    }
    CHECK(worst <= 8);
}

void testIsoFormats(const std::string& dir) {
    demo::DiscSpec spec;
    spec.gameId = "GMPP01";
    spec.revision = 1;
    spec.bnr2 = true;
    spec.art = 1;
    const BannerText english{"Mario Party 4", "Nintendo", "Mario Party 4 EN", "Hudson", "English text"};
    const BannerText french{"Mario Party 4", "Nintendo", "Mario Party 4 FR", "Hudson", "Texte fran\xE7" "ais"};
    spec.texts = {english, english, french, english, english, english};
    const std::vector<uint8_t> iso = demo::buildIso(spec);

    struct Case {
        const char* name;
        std::vector<uint8_t> bytes;
        DiscFormat format;
        bool banner;
    };
    const Case cases[] = {
        {"disc.iso", iso, DiscFormat::Iso, true},
        {"disc.gcm", iso, DiscFormat::Iso, true},
        {"disc.ciso", demo::wrapCiso(iso, 0x8000), DiscFormat::Ciso, true},
        {"disc.gcz", demo::wrapGcz(iso, 0x4000), DiscFormat::Gcz, true},
        {"disc.rvz", demo::wrapRvzHeader(iso), DiscFormat::Rvz, false},
    };
    for (const Case& c : cases) {
        const std::string path = dir + "/" + c.name;
        CHECK(demo::writeFile(path, c.bytes));
        DiscInfo info;
        CHECK(readDisc(path, info) == DiscError::None);
        CHECK(info.format == c.format);
        CHECK_EQ(info.gameId, "GMPP01");
        CHECK_EQ(info.makerCode, "01");
        CHECK_EQ(info.revision, 1);
        CHECK_EQ(info.internalTitle, "Mario Party 4");
        CHECK_EQ(info.regionCode(), 'P');
        CHECK_EQ(info.hasBanner(), c.banner);
        if (c.banner) {
            expectDemoBanner(info, 1);
            CHECK_EQ(info.bannerText.size(), 6u);
            const BannerText* fr = info.text(BannerLanguage::French);
            CHECK(fr != nullptr);
            if (fr) {
                CHECK_EQ(fr->longTitle, "Mario Party 4 FR");
                CHECK_EQ(fr->description, "Texte fran\xC3\xA7" "ais");
            }
            const BannerText* de = info.text(BannerLanguage::German);
            CHECK(de && de->longTitle == "Mario Party 4 EN");
        }
    }

    // A file that is not a GameCube disc, and a Wii-style header.
    CHECK(demo::writeFile(dir + "/zero.iso", std::vector<uint8_t>(0x1000, 0)));
    DiscInfo info;
    CHECK(readDisc(dir + "/zero.iso", info) == DiscError::NotGameCube);
    CHECK(readDisc(dir + "/missing.iso", info) == DiscError::Open);
    CHECK(demo::writeFile(dir + "/fake.ciso", std::vector<uint8_t>(0x9000, 1)));
    CHECK(readDisc(dir + "/fake.ciso", info) == DiscError::UnsupportedContainer);
    CHECK(discFormatFromPath("a/b.ISO") == DiscFormat::Iso);
    CHECK(discFormatFromPath("a.b/readme") == DiscFormat::Unknown);
    CHECK(discFormatFromPath("x.nkit.iso") == DiscFormat::Iso);
}

void testBnr1Fallbacks(const std::string& dir) {
    demo::DiscSpec spec;
    spec.gameId = "GMPE01";
    spec.texts = {{"MP4", "Nintendo", "", "Hudson", "Party"}};
    CHECK(demo::writeFile(dir + "/usa.iso", demo::buildIso(spec)));
    DiscInfo info;
    CHECK(readDisc(dir + "/usa.iso", info) == DiscError::None);
    CHECK_EQ(info.bannerText.size(), 1u);
    // French requested on a BNR1 disc: the only block is used.
    const BannerText* text = info.text(BannerLanguage::French);
    CHECK(text && text->shortTitle == "MP4");
}

Catalog shippedCatalog() {
    Catalog catalog;
    CHECK(catalog.loadFile(std::string(PARTYBOARD_LAUNCHER_ASSETS_DIR) + "/catalog.json"));
    return catalog;
}

void testLibrary() {
    const std::string root = tempDir();
    demo::makeDemoSdCard(root, true);
    const SdLayout layout = sdLayout(root);
    const Catalog catalog = shippedCatalog();
    const std::vector<GameEntry> games =
        scanLibrary(layout.gameDirectories, layout.coversDirectory, Language::French, catalog);

    // USA, PAL (sub-folder), Strikers, Japan (second folder), test disc,
    // corrupt. The AppleDouble "._" file is skipped.
    CHECK_EQ(games.size(), 6u);
    if (games.size() != 6u)
        return;
    CHECK(games[0].compatibility == Compatibility::Supported);
    CHECK(games[1].compatibility == Compatibility::Supported);
    CHECK(games[2].compatibility == Compatibility::NoSwitchRuntime);
    CHECK(games[3].compatibility == Compatibility::UnsupportedRegion);
    CHECK(games[4].compatibility == Compatibility::OtherGame);
    CHECK(games[5].compatibility == Compatibility::Unreadable);

    int pal = -1;
    for (int i = 0; i < 2; ++i) {
        if (games[i].disc.gameId == "GMPP01")
            pal = i;
    }
    CHECK(pal >= 0);
    if (pal >= 0) {
        // Catalogue description first, in the launcher's language.
        CHECK(games[pal].description(Language::French).find("PartyBoard") != std::string::npos);
        CHECK(games[pal].description(Language::English).find("natively") != std::string::npos);
        CHECK(games[pal].disc.format == DiscFormat::Ciso);
        CHECK(games[pal].catalog && games[pal].catalog->coverFor('P')->front == "covers/mp4_pal_front.png");
        // European box art for every release, the US disc included.
        const GameEntry& usa = games[1 - pal];
        CHECK(usa.catalog && usa.catalog->coverFor(usa.disc.regionCode())->front == "covers/mp4_pal_front.png");
    }
    // The PAL title of Super Mario Strikers comes from regionTitles.
    CHECK_EQ(games[2].title(Language::French), "Mario Smash Football");
    CHECK(games[2].catalog && games[2].catalog->runtime == "Strikers");
    CHECK(games[2].catalog && games[2].catalog->engines.empty());
    // Japanese banner text is Shift-JIS: the catalogue title is used.
    CHECK_EQ(games[3].title(Language::French), "Mario Party 4");
    CHECK(games[3].disc.format == DiscFormat::Gcz);
    CHECK_EQ(games[4].title(Language::French), "PartyBoard Test Disc");
    CHECK(games[4].catalog == nullptr);
    CHECK(!games[4].coverPath.empty());
    CHECK_EQ(games[5].title(Language::French), "corrupt");
    CHECK(!games[5].launchable());
    CHECK(games[0].launchable());

    // Without a catalogue file the launcher still knows Mario Party 4.
    const Catalog builtin = Catalog::builtin();
    const std::vector<GameEntry> fallback =
        scanLibrary(layout.gameDirectories, layout.coversDirectory, Language::French, builtin);
    CHECK_EQ(fallback.size(), 6u);
    if (fallback.size() == 6u) {
        CHECK(fallback[0].launchable() && fallback[1].launchable());
        CHECK(fallback[2].compatibility == Compatibility::UnsupportedRegion);
    }
}

void testCatalog() {
    const char* document = R"({
      "games": [
        {"id": "GMPE01_00", "title": "Mario Party 4", "discs": ["GMPE01_00"], "recognised": ["GMPJ01"],
         "engines": ["switch/partyboard.nro"], "genre": {"fr": "Fête", "en": "Party"}, "players": "1-4",
         "covers": {"E": {"front": "us.png"}, "*": {"front": "any.png", "spine": "s.png"}}},
        {"id": "G4QE01", "title": "Super Mario Strikers", "regionTitles": {"P": "Mario Smash Football"},
         "discs": ["G4QE01", "G4QP01"]}
      ]})";
    json::Value value;
    CHECK(json::parse(document, value));
    Catalog catalog;
    CHECK(catalog.load(value));
    CHECK_EQ(catalog.games().size(), 2u);

    DiscInfo disc;
    disc.gameId = "GMPE01";
    disc.revision = 0;
    DiscMatch match;
    CHECK_EQ(catalog.find(disc, match), 0);
    CHECK(match == DiscMatch::Accepted);
    CHECK_EQ(revisionId(disc), "GMPE01_00");
    disc.revision = 1; // only revision 0 is listed here
    CHECK_EQ(catalog.find(disc, match), 0);
    CHECK(match == DiscMatch::OtherRevision);
    CHECK(classifyDisc(disc, catalog, nullptr) == Compatibility::UnsupportedRevision);
    disc.gameId = "GMPJ01";
    CHECK_EQ(catalog.find(disc, match), 0);
    CHECK(match == DiscMatch::Recognised);
    disc.gameId = "G4QP01";
    CHECK_EQ(catalog.find(disc, match), 1);
    CHECK(classifyDisc(disc, catalog, nullptr) == Compatibility::NoSwitchRuntime);
    disc.gameId = "GALE01";
    CHECK_EQ(catalog.find(disc, match), -1);
    CHECK(match == DiscMatch::None);

    const CatalogEntry& mp4 = catalog.games()[0];
    CHECK_EQ(mp4.genre.get(Language::French), "Fête");
    CHECK_EQ(mp4.genre.get(Language::English), "Party");
    CHECK_EQ(mp4.players.get(Language::French), "1-4");
    CHECK_EQ(mp4.coverFor('E')->front, "us.png");
    CHECK_EQ(mp4.coverFor('F')->front, "any.png"); // French PAL disc, no P cover: the wildcard
    CHECK_EQ(catalog.games()[1].titleFor('P'), "Mario Smash Football");
    CHECK_EQ(catalog.games()[1].titleFor('E'), "Super Mario Strikers");
    CHECK(catalog.games()[1].coverFor('E') == nullptr);

    // A user catalogue replaces an entry by id and appends new ones.
    json::Value extra;
    CHECK(json::parse(R"([{"id": "G4QE01", "title": "Strikers Switch", "discs": ["G4QE01"],
                          "engines": ["switch/strikers.nro"]},
                         {"id": "GALE01", "title": "Other", "discs": ["GALE01"]}])", extra));
    CHECK(catalog.load(extra));
    CHECK_EQ(catalog.games().size(), 3u);
    CHECK_EQ(catalog.games()[1].title, "Strikers Switch");
    disc.gameId = "G4QE01";
    CHECK(classifyDisc(disc, catalog, nullptr) == Compatibility::Supported);

    // Every cover the shipped catalogue names exists in assets/.
    const Catalog shipped = shippedCatalog();
    CHECK_EQ(shipped.games().size(), 3u);
    for (const CatalogEntry& entry : shipped.games()) {
        for (const auto& [region, art] : entry.covers) {
            Image image;
            CHECK(loadPng(std::string(PARTYBOARD_LAUNCHER_ASSETS_DIR) + "/" + art.front, image));
            CHECK(art.spine.empty() || loadPng(std::string(PARTYBOARD_LAUNCHER_ASSETS_DIR) + "/" + art.spine, image));
        }
    }
}

void testJson() {
    json::Value value;
    CHECK(json::parse("\xEF\xBB\xBF{\"a\": [1, 2.5, -3e2, true, false, null], \"s\": \"\\u00e9\\n\\ud83c\\udf89\\\"\"}", value));
    CHECK(value.isObject());
    CHECK_EQ(value["a"].items().size(), 6u);
    CHECK_EQ(value["a"].items()[1].asNumber(), 2.5);
    CHECK_EQ(value["a"].items()[2].asInteger(), -300);
    CHECK_EQ(value["a"].items()[3].asBool(), true);
    CHECK(value["a"].items()[5].isNull());
    CHECK_EQ(value["s"].asString(), "\xC3\xA9\n\xF0\x9F\x8E\x89\"");
    CHECK(value["missing"].isNull());

    json::Value round;
    CHECK(json::parse(json::serialize(value), round));
    CHECK_EQ(round["s"].asString(), value["s"].asString());
    CHECK_EQ(round["a"].items()[1].asNumber(), 2.5);

    std::string error;
    CHECK(!json::parse("{\"a\": }", value, &error));
    CHECK(!error.empty());
    CHECK(!json::parse("[1, 2", value));
    CHECK(!json::parse("{\"a\": 1} trailing", value));
    CHECK(!json::parse("\"tab\there\"", value));
    std::string deep(200, '[');
    CHECK(!json::parse(deep, value));
}

void testMods() {
    const std::string root = tempDir();
    demo::makeDemoSdCard(root, false);
    const SdLayout layout = sdLayout(root);

    // CubeShelf names the folder after the revision id; the bare id is not there.
    const std::string dir = ModSet::findDirectory(layout.modsDirectory, {"GMPE01_00", "GMPE01"});
    CHECK_EQ(dir, layout.modsDirectory + "/GMPE01_00");
    CHECK(ModSet::findDirectory(layout.modsDirectory, {"GMPE01", "../GMPE01_00"}).empty());

    ModSet mods;
    CHECK(mods.load(dir));
    CHECK_EQ(mods.mods().size(), 4u);
    if (mods.mods().size() != 4u)
        return;
    // Highest priority first, Windows paths re-rooted on the SD card.
    CHECK_EQ(mods.mods()[0].id, 546878);
    CHECK_EQ(mods.mods()[0].resolvedRoot, dir + "/546878/files");
    CHECK(mods.mods()[0].present && mods.mods()[0].active());
    CHECK(mods.mods()[1].playerDisabled && !mods.mods()[1].active());
    CHECK(!mods.mods()[2].enabled);
    CHECK(!mods.mods()[3].present && !mods.mods()[3].active());
    CHECK_EQ(mods.activeCount(), 1u);

    auto readText = [](const std::string& path) {
        std::string text;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return text;
        char buffer[512];
        size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
            text.append(buffer, got);
        std::fclose(f);
        return text;
    };

    CHECK_EQ(mods.writeActiveList(), dir + "/active-mods.txt");
    CHECK_EQ(readText(dir + "/active-mods.txt"), dir + "/546878/files\n");

    // Switching on a mod the game had switched off clears the in-game switch.
    mods.toggle(1);
    CHECK(mods.mods()[1].active());
    CHECK_EQ(mods.activeCount(), 2u);
    // Move it above the first: it now wins conflicts.
    CHECK(mods.move(1, -1));
    CHECK_EQ(mods.mods()[0].id, 407132);
    CHECK(!mods.move(0, -1));
    mods.writeActiveList();
    CHECK_EQ(readText(dir + "/active-mods.txt"), dir + "/407132/files\n" + dir + "/546878/files\n");

    // Saved like CubeShelf would read it back: original paths kept.
    CHECK(mods.save());
    ModSet reloaded;
    CHECK(reloaded.load(dir));
    CHECK_EQ(reloaded.mods()[0].id, 407132);
    CHECK(!reloaded.mods()[0].playerDisabled);
    CHECK(reloaded.mods()[0].contentRoot.find("C:\\Users\\Player") == 0);
    CHECK_EQ(readText(dir + "/player-disabled.json"), "[]\n");

    // Turning every mod off leaves an empty list and no --mod-list.
    for (size_t i = 0; i < reloaded.mods().size(); ++i) {
        if (reloaded.mods()[i].active())
            reloaded.toggle(i);
    }
    CHECK(reloaded.writeActiveList().empty());
    CHECK_EQ(readText(dir + "/active-mods.txt"), "");

    Settings settings;
    const std::vector<std::string> args =
        buildLaunchArgs("sdmc:/e.nro", "sdmc:/g.iso", "", settings, Language::English, dir + "/active-mods.txt");
    CHECK_EQ(args.back(), "--mod-list=" + dir + "/active-mods.txt");
}

void testSettings() {
    const std::string dir = tempDir();
    const std::string path = dir + "/nested/config/launcher.ini";
    Settings out;
    out.bootAnimation = false;
    out.aspect = AspectMode::Wide169;
    out.filter = ScreenFilter::Scanlines;
    out.language = LanguagePref::English;
    out.rumble = false;
    out.lastGame = "sdmc:/partyboard/games/Mario Party 4 (USA).iso";
    CHECK(saveSettings(path, out));

    Settings in;
    CHECK(loadSettings(path, in));
    CHECK_EQ(in.bootAnimation, false);
    CHECK(in.aspect == AspectMode::Wide169);
    CHECK(in.filter == ScreenFilter::Scanlines);
    CHECK(in.language == LanguagePref::English);
    CHECK_EQ(in.rumble, false);
    CHECK_EQ(in.lastGame, out.lastGame);
    CHECK(in.resolveLanguage(Language::French) == Language::English);
    Settings automatic;
    CHECK(automatic.resolveLanguage(Language::French) == Language::French);

    const std::vector<std::string> args =
        buildLaunchArgs("sdmc:/switch/partyboard/partyboard.nro", out.lastGame,
                        "sdmc:/switch/partyboard-launcher.nro", out, Language::French);
    CHECK_EQ(args.size(), 7u);
    CHECK_EQ(args[1], "--disc-image=sdmc:/partyboard/games/Mario Party 4 (USA).iso");
    CHECK_EQ(args[2], "--aspect=wide");
    CHECK_EQ(args[3], "--filter=crt");
    CHECK_EQ(args[4], "--lang=fr");
    CHECK_EQ(args[5], "--rumble=off");
    CHECK_EQ(joinArgv({"sdmc:/a.nro", "--x=1", "--disc-image=sdmc:/a b.iso"}),
             "sdmc:/a.nro --x=1 \"--disc-image=sdmc:/a b.iso\"");
}

void testBootTimeline() {
    const std::vector<BootCue> cues = BootAnimation::cues(false);
    CHECK(!cues.empty());
    for (size_t i = 1; i < cues.size(); ++i)
        CHECK(cues[i].time >= cues[i - 1].time);
    CHECK(cues.back().sound == Sound::Chime);
    CHECK(std::fabs(cues.back().time - BootAnimation::kSlamTime) < 1e-9);
    CHECK(BootAnimation::cues(true).back().sound == Sound::ChimeAlt);

    const BootFrame start = BootAnimation::evaluate(0.0, false, 16.0f / 9.0f);
    CHECK(start.cubes.empty());
    CHECK_EQ(start.fadeIn, 0.0f);
    const BootFrame done = BootAnimation::evaluate(4.0, false, 16.0f / 9.0f);
    CHECK_EQ(done.cubes.size(), 8u); // seven trail cubes and the core
    CHECK(done.logoAlpha > 0.99f);
    CHECK_EQ(done.blackout, 0.0f);
    CHECK(BootAnimation::evaluate(BootAnimation::kDuration, false, 1.0f).blackout > 0.99f);

    // Cubes stay on the 3x3 floor (no NaNs, nothing flung away).
    for (double t = 0.0; t <= BootAnimation::kDuration; t += 1.0 / 60.0) {
        const BootFrame frame = BootAnimation::evaluate(t, (static_cast<int>(t * 10) % 2) != 0, 16.0f / 9.0f);
        for (const BootCube& cube : frame.cubes) {
            const Vec3 p = cube.model.transformPoint({0, 0, 0});
            CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
            CHECK(std::fabs(p.x) <= 1.6f && std::fabs(p.z) <= 1.6f && p.y <= 8.6f && p.y >= -0.2f);
        }
    }
}

void testSounds() {
    SoundBank bank;
    bank.generate();
    for (size_t i = 0; i < static_cast<size_t>(Sound::Count); ++i) {
        const std::vector<int16_t>& clip = bank.clips[i];
        CHECK(!clip.empty());
        CHECK(clip.size() % 2 == 0);
        int peak = 0;
        for (int16_t v : clip)
            peak = std::max(peak, std::abs(int(v)));
        CHECK(peak > 1000);
    }
    CHECK(bank.clip(Sound::Chime).size() / 2 > static_cast<size_t>(kSampleRate * 2));

    Mixer mixer(bank);
    std::vector<int16_t> out(1024 * 2, 1);
    mixer.mix(out.data(), 1024);
    CHECK(std::all_of(out.begin(), out.end(), [](int16_t v) { return v == 0; }));
    mixer.trigger(Sound::Select);
    mixer.mix(out.data(), 1024);
    CHECK(std::any_of(out.begin(), out.end(), [](int16_t v) { return v != 0; }));
    // Overfilling the command ring drops triggers instead of corrupting it.
    for (int i = 0; i < 200; ++i)
        mixer.trigger(Sound::Move);
    mixer.mix(out.data(), 1024);
}

void testResources() {
    // The artwork the Switch build packs into romfs must decode with the
    // launcher's own PNG loader.
    const std::string res = PARTYBOARD_LAUNCHER_RES_DIR;
    const struct {
        const char* name;
        int width;
        int height;
    } expected[] = {{"logo.png", 1009, 160}, {"icon.png", 512, 512}, {"prelaunch-bg.png", 1700, 1080}};
    for (const auto& e : expected) {
        Image image;
        CHECK(loadPng(res + "/" + e.name, image, 2048));
        CHECK_EQ(image.width, e.width);
        CHECK_EQ(image.height, e.height);
        CHECK_EQ(image.rgba.size(), size_t(e.width) * e.height * 4);
    }
    Image small;
    CHECK(loadPng(res + "/prelaunch-bg.png", small, 1024));
    CHECK(small.width <= 1024 && small.height <= 1024 && small.width == 850);
}

} // namespace

int main() {
    const std::string dir = tempDir();
    testTextConversion();
    testRgb5a3();
    testIsoFormats(dir);
    testBnr1Fallbacks(dir);
    testLibrary();
    testCatalog();
    testJson();
    testMods();
    testSettings();
    testBootTimeline();
    testSounds();
    testResources();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
