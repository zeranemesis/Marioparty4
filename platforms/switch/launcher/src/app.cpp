#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>

#include "image.hpp"
#include "paths.hpp"

namespace partyboard::launcher {

namespace {

constexpr float W = Renderer::kWidth;
constexpr float H = Renderer::kHeight;

// GameCube indigo, with the Switch's cyan selection highlight on top.
constexpr Color kBgTop = rgb(0x2A2266);
constexpr Color kBgBottom = rgb(0x0D0A26);
// PartyBoard's selection cyan, from the PC pre-launch menu (#009dda).
constexpr Color kAccent = rgb(0x009DDA);
constexpr Color kAccentLight = rgb(0x8FE8FF);
constexpr Color kText = rgb(0xFFFFFF);
constexpr Color kTextSoft = rgb(0xC9C3F5);
constexpr Color kTextDim = rgb(0x8F88C9);
constexpr Color kPanel = rgb(0x1B1550);
constexpr Color kPanelRow = rgb(0x2A2370);
constexpr Color kInk = rgb(0x1B1446);
constexpr Color kGreen = rgb(0x48D38A);
constexpr Color kOrange = rgb(0xFFB347);
constexpr Color kRed = rgb(0xFF6B7A);
constexpr Color kGrey = rgb(0xA6A1D1);
constexpr Color kPlayerColors[4] = {rgb(0xFF4D6D), rgb(0x3B82FF), rgb(0xFFD23B), rgb(0x3BD67A)};

constexpr float kCardHeight = 340.0f;
constexpr float kCardAspect = 0.72f; // GameCube case
constexpr float kShelfBaseline = 468.0f;

constexpr int kOptionRows = 5;

constexpr uint32_t kDirections = kButtonUp | kButtonDown | kButtonLeft | kButtonRight;

bool fileExists(const std::string& path) {
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

Str regionName(char code) {
    switch (code) {
    case 'E': return Str::RegionUsa;
    case 'J': return Str::RegionJapan;
    case 'P': case 'D': case 'F': case 'S': case 'I': case 'U': case 'X': case 'Y': return Str::RegionEurope;
    default: return Str::RegionOther;
    }
}

Str statusText(Compatibility compatibility) {
    switch (compatibility) {
    case Compatibility::Supported: return Str::StatusSupported;
    case Compatibility::NoSwitchRuntime: return Str::StatusNoSwitchRuntime;
    case Compatibility::UnsupportedRevision: return Str::StatusUnsupportedRevision;
    case Compatibility::UnsupportedRegion: return Str::StatusUnsupportedRegion;
    case Compatibility::OtherGame: return Str::StatusOtherGame;
    case Compatibility::Unreadable: break;
    }
    return Str::StatusUnreadable;
}

Color statusColor(Compatibility compatibility) {
    switch (compatibility) {
    case Compatibility::Supported: return kGreen;
    case Compatibility::NoSwitchRuntime: return kAccentLight;
    case Compatibility::UnsupportedRevision:
    case Compatibility::UnsupportedRegion: return kOrange;
    case Compatibility::OtherGame: return kGrey;
    case Compatibility::Unreadable: break;
    }
    return kRed;
}

// Stable per-game hue for generated covers of discs we know nothing about.
Color hashedColor(const std::string& key, float lightness) {
    uint32_t h = 2166136261u;
    for (char c : key)
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    const float hue = static_cast<float>(h % 360) / 360.0f;
    auto channel = [&](float offset) {
        return lightness * (0.55f + 0.45f * std::cos(6.2831853f * (hue + offset)));
    };
    return {channel(0.0f), channel(0.33f), channel(0.67f), 1.0f};
}

// Mario Party 4 discs get the PartyBoard case art; other discs a generated case.
bool isMarioParty4(const GameEntry& game) {
    return game.error == DiscError::None && game.disc.gameId.rfind("GMP", 0) == 0;
}

std::string format(const char* pattern, unsigned value) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

} // namespace

App::App(Platform& platform, Renderer& renderer) : m_platform(platform), m_r(renderer) {}

App::~App() {
    releaseTextures();
    m_r.destroyTexture(m_logo);
    m_r.destroyTexture(m_star);
    m_r.destroyTexture(m_art);
    m_r.destroyTexture(m_gcLogo);
    for (auto& [path, texture] : m_artCache)
        m_r.destroyTexture(texture);
}

void App::init() {
    loadSettings(m_platform.layout().settingsPath, m_settings);
    m_systemLanguage = m_platform.systemLanguage();
    m_language = m_settings.resolveLanguage(m_systemLanguage);
    m_now = m_last = m_platform.now();
    m_shelfFadeStart = m_now;
    loadArtwork();
    loadCatalog();
    rescan(false);
}

void App::loadCatalog() {
    // Shipped catalogue first, then the player's additions on the SD card.
    if (!m_catalog.loadFile(m_platform.resourcePath("catalog.json")) || m_catalog.empty())
        m_catalog = Catalog::builtin();
    m_catalog.loadFile(m_platform.layout().userCatalogPath);
}

const Texture* App::cachedArt(const std::string& resource) {
    if (resource.empty())
        return nullptr;
    auto it = m_artCache.find(resource);
    if (it == m_artCache.end()) {
        Image image;
        Texture texture;
        if (loadPng(m_platform.resourcePath(resource), image, 1024))
            texture = m_r.createTexture(image.width, image.height, image.rgba.data(), true);
        it = m_artCache.emplace(resource, texture).first;
    }
    return it->second ? &it->second : nullptr;
}

void App::loadArtwork() {
    // Missing files are tolerated: every use falls back to drawn shapes.
    auto load = [this](const char* name, bool mipmaps) {
        Image image;
        if (!loadPng(m_platform.resourcePath(name), image, 2048))
            return Texture{};
        return m_r.createTexture(image.width, image.height, image.rgba.data(), mipmaps);
    };
    m_logo = load("logo.png", true);
    m_star = load("icon.png", true);
    m_art = load("prelaunch-bg.png", true);
    m_gcLogo = load("gamecube_logo.png", true);
}

void App::releaseTextures() {
    for (GameVisual& visual : m_visuals) {
        m_r.destroyTexture(visual.banner);
        m_r.destroyTexture(visual.cover);
    }
    m_visuals.clear();
}

void App::rescan(bool announce) {
    const std::string previous = m_selected < m_games.size() ? m_games[m_selected].path : m_settings.lastGame;
    releaseTextures();
    const SdLayout& layout = m_platform.layout();
    m_games = scanLibrary(layout.gameDirectories, layout.coversDirectory, m_language, m_catalog);
    m_visuals.resize(m_games.size());
    for (size_t i = 0; i < m_games.size(); ++i) {
        const GameEntry& game = m_games[i];
        if (game.disc.hasBanner())
            m_visuals[i].banner = m_r.createTexture(kBannerWidth, kBannerHeight, game.disc.bannerRgba.data(), false);
        Image cover;
        if (!game.coverPath.empty() && loadPng(game.coverPath, cover))
            m_visuals[i].cover = m_r.createTexture(cover.width, cover.height, cover.rgba.data(), true);
        if (game.catalog) {
            if (const CoverArt* art = game.catalog->coverFor(game.disc.regionCode())) {
                m_visuals[i].front = cachedArt(art->front);
                m_visuals[i].spine = cachedArt(art->spine);
            }
        }
    }
    m_selectedModsFor = static_cast<size_t>(-1);

    m_selected = 0;
    for (size_t i = 0; i < m_games.size(); ++i) {
        if (m_games[i].path == previous)
            m_selected = i;
    }
    m_scroll = static_cast<float>(m_selected);
    if (announce)
        showToast(format(t(Str::GamesFound), static_cast<unsigned>(m_games.size())));
}

void App::refreshLanguage() {
    m_language = m_settings.resolveLanguage(m_systemLanguage);
}

void App::persistSettings() { saveSettings(m_platform.layout().settingsPath, m_settings); }

void App::openOverlay(Overlay overlay) {
    m_overlay = overlay;
    m_overlayDrawn = overlay;
    m_platform.playSound(overlay == Overlay::Dialog ? Sound::Error : Sound::Open, 0.8f);
}

void App::closeOverlay() {
    if (m_overlay == Overlay::Options)
        persistSettings();
    if (m_overlay == Overlay::Mods)
        closeMods();
    m_overlay = Overlay::None;
    m_platform.playSound(Sound::Back, 0.8f);
}

bool App::hasMods(const GameEntry& game) const {
    return game.launchable() && game.catalog && game.catalog->mods;
}

std::string App::labelled(Str label, const std::string& value) const {
    // French typography puts a space before the colon.
    return std::string(t(label)) + (m_language == Language::French ? " : " : ": ") + value;
}

void App::openMods() {
    const GameEntry& game = m_games[m_selected];
    const std::vector<std::string> candidates = modDirectoryCandidates(game);
    const std::string& root = m_platform.layout().modsDirectory;
    m_modsDirectory = ModSet::findDirectory(root, candidates);
    m_mods = ModSet();
    if (!m_modsDirectory.empty())
        m_mods.load(m_modsDirectory);
    else if (!candidates.empty())
        m_modsDirectory = root + "/" + (game.catalog ? game.catalog->id : candidates.front());
    m_modRow = 0;
    m_modsDirty = false;
    openOverlay(Overlay::Mods);
}

void App::closeMods() {
    if (m_modsDirty) {
        m_mods.save();
        m_mods.writeActiveList();
        m_modsDirty = false;
    }
    m_selectedModsFor = static_cast<size_t>(-1); // refresh the count under the shelf
}

void App::updateMods(const InputState& input, uint32_t nav) {
    const int count = static_cast<int>(m_mods.mods().size());
    if (nav & kButtonUp && m_modRow > 0) {
        --m_modRow;
        m_platform.playSound(Sound::Move, 0.7f);
    }
    if (nav & kButtonDown && m_modRow + 1 < count) {
        ++m_modRow;
        m_platform.playSound(Sound::Move, 0.7f);
    }
    if ((input.pressed & kButtonA) && m_modRow < count) {
        m_mods.toggle(static_cast<size_t>(m_modRow));
        m_modsDirty = true;
        m_platform.playSound(Sound::Select, 0.7f);
    }
    // L/R move the focused mod up or down the load order, and focus follows it.
    for (const auto& [button, direction] : {std::pair<uint32_t, int>{kButtonL, -1}, {kButtonR, 1}}) {
        if ((input.pressed & button) && m_modRow < count) {
            const int id = m_mods.mods()[static_cast<size_t>(m_modRow)].id;
            if (m_mods.move(static_cast<size_t>(m_modRow), direction)) {
                m_modsDirty = true;
                for (int i = 0; i < count; ++i) {
                    if (m_mods.mods()[static_cast<size_t>(i)].id == id)
                        m_modRow = i;
                }
                m_platform.playSound(Sound::Move, 0.9f);
            }
        }
    }
    if (input.pressed & kButtonB)
        closeOverlay();
}

void App::showDialog(Str title, Str body, std::string detail) {
    m_dialogTitle = t(title);
    m_dialogBody = t(body);
    m_dialogDetail = std::move(detail);
    openOverlay(Overlay::Dialog);
}

void App::showToast(std::string message) {
    m_toast = std::move(message);
    m_toastUntil = m_now + 2.6;
}

std::string App::findEngine(const CatalogEntry* entry) const {
    if (!entry)
        return {};
    // launcher.ini's engine= overrides PartyBoard's location only.
    if (entry->runtime == "PartyBoard" && fileExists(m_settings.enginePath))
        return m_settings.enginePath;
    for (const std::string& relative : entry->engines) {
        const std::string candidate = m_platform.layout().root + "/" + relative;
        if (fileExists(candidate))
            return candidate;
    }
    return {};
}

std::vector<std::string> App::modDirectoryCandidates(const GameEntry& game) const {
    // CubeShelf names the folder after the catalogue revision id (GMPE01_00).
    // Only for the same game id: mods are built against one release's files.
    std::vector<std::string> names;
    if (game.error != DiscError::None)
        return names;
    names.push_back(revisionId(game.disc));
    if (game.catalog && game.catalog->id.compare(0, 6, game.disc.gameId) == 0)
        names.push_back(game.catalog->id);
    names.push_back(game.disc.gameId);
    return names;
}

void App::requestLaunch() {
    if (m_selected >= m_games.size()) {
        m_platform.playSound(Sound::Error);
        return;
    }
    const GameEntry& game = m_games[m_selected];
    if (game.compatibility == Compatibility::NoSwitchRuntime) {
        showDialog(Str::LaunchNoRuntimeTitle, Str::LaunchNoRuntimeBody,
                   labelled(Str::RuntimeLabel, game.catalog->runtime));
        return;
    }
    if (!game.launchable()) {
        showDialog(Str::LaunchUnsupportedTitle, Str::LaunchUnsupportedBody, t(statusText(game.compatibility)));
        return;
    }
    const std::string engine = findEngine(game.catalog);
    if (engine.empty()) {
        const std::vector<std::string>& engines = game.catalog->engines;
        showDialog(Str::LaunchEngineMissingTitle, Str::LaunchEngineMissingBody,
                   engines.empty() ? std::string{} : m_platform.layout().root + "/" + engines.front());
        return;
    }

    // Like CubeShelf right before it starts PartyBoard: rewrite the active
    // list from installed.json so the engine never reads a stale one.
    std::string modList;
    if (game.catalog->mods) {
        ModSet mods;
        const std::string dir = ModSet::findDirectory(m_platform.layout().modsDirectory, modDirectoryCandidates(game));
        if (!dir.empty() && mods.load(dir))
            modList = mods.writeActiveList();
    }

    m_settings.lastGame = game.path;
    persistSettings();
    m_launchArgs = buildLaunchArgs(engine, game.path, m_platform.selfPath(), m_settings, m_language, modList);
    m_platform.playSound(Sound::Select);
    m_launchStart = m_now;
    if (m_settings.bootAnimation) {
        m_screen = Screen::Boot;
        m_boot.start(m_now);
    } else {
        m_screen = Screen::Launching;
    }
}

void App::finishLaunch() {
    if (m_platform.launch(m_launchArgs) == LaunchResult::Scheduled) {
        m_quit = true;
        return;
    }
    m_screen = Screen::Shelf;
    m_shelfFadeStart = m_now;
    showDialog(Str::LaunchFailedTitle, Str::LaunchFailedBody, m_launchArgs.empty() ? std::string{} : m_launchArgs[0]);
}

uint32_t App::navigation(const InputState& input) {
    uint32_t held = input.held & kDirections;
    if (std::fabs(input.stickX) > 0.55f || std::fabs(input.stickY) > 0.55f) {
        if (std::fabs(input.stickX) > std::fabs(input.stickY))
            held |= input.stickX > 0 ? kButtonRight : kButtonLeft;
        else
            held |= input.stickY > 0 ? kButtonUp : kButtonDown;
    }
    uint32_t primary = 0;
    for (uint32_t bit : {kButtonUp, kButtonDown, kButtonLeft, kButtonRight}) {
        if (held & bit) {
            primary = bit;
            break;
        }
    }
    if (primary != m_repeatDir) {
        m_repeatDir = primary;
        m_repeatSince = m_repeatLast = m_now;
        return primary;
    }
    if (primary && m_now - m_repeatSince > 0.38 && m_now - m_repeatLast > 0.085) {
        m_repeatLast = m_now;
        return primary;
    }
    return 0;
}

void App::updateShelf(const InputState& input, uint32_t nav) {
    if (nav & (kButtonLeft | kButtonRight)) {
        const size_t before = m_selected;
        if ((nav & kButtonLeft) && m_selected > 0)
            --m_selected;
        if ((nav & kButtonRight) && m_selected + 1 < m_games.size())
            ++m_selected;
        if (m_selected != before)
            m_platform.playSound(Sound::Move, 0.7f);
    }
    if (input.pressed & kButtonA)
        requestLaunch();
    else if (input.pressed & kButtonX)
        openOverlay(Overlay::Options);
    else if (input.pressed & kButtonY)
        openOverlay(Overlay::Controllers);
    else if ((input.pressed & kButtonR) && m_selected < m_games.size() && hasMods(m_games[m_selected]))
        openMods();
    else if (input.pressed & kButtonMinus)
        rescan(true);
    else if (input.pressed & kButtonPlus)
        m_quit = true;
}

void App::cycleOption(int row, int delta) {
    auto wrap3 = [delta](int value) { return (value + delta + 3) % 3; };
    switch (row) {
    case 0: m_settings.bootAnimation = !m_settings.bootAnimation; break;
    case 1: m_settings.aspect = static_cast<AspectMode>(wrap3(static_cast<int>(m_settings.aspect))); break;
    case 2: m_settings.filter = static_cast<ScreenFilter>(wrap3(static_cast<int>(m_settings.filter))); break;
    case 3:
        m_settings.language = static_cast<LanguagePref>(wrap3(static_cast<int>(m_settings.language)));
        refreshLanguage();
        break;
    case 4: m_settings.rumble = !m_settings.rumble; break;
    default: break;
    }
    m_platform.playSound(Sound::Move, 0.8f);
}

void App::updateOptions(const InputState& input, uint32_t nav) {
    if (nav & kButtonUp && m_optionRow > 0) {
        --m_optionRow;
        m_platform.playSound(Sound::Move, 0.7f);
    }
    if (nav & kButtonDown && m_optionRow + 1 < kOptionRows) {
        ++m_optionRow;
        m_platform.playSound(Sound::Move, 0.7f);
    }
    if (nav & kButtonLeft)
        cycleOption(m_optionRow, -1);
    if ((nav & kButtonRight) || (input.pressed & kButtonA))
        cycleOption(m_optionRow, 1);
    if (input.pressed & (kButtonB | kButtonX))
        closeOverlay();
}

void App::updateControllers(const InputState& input, uint32_t nav) {
    constexpr int kRows = 11;
    if (nav & kButtonUp && m_mapRow > 0) {
        --m_mapRow;
        m_platform.playSound(Sound::Move, 0.7f);
    }
    if (nav & kButtonDown && m_mapRow + 1 < kRows) {
        ++m_mapRow;
        m_platform.playSound(Sound::Move, 0.7f);
    }
    if (input.pressed & kButtonY) {
        m_platform.playSound(Sound::Select, 0.8f);
        if (!m_platform.showControllerApplet())
            showToast(t(Str::ControllersAppletFailed));
        m_slots = m_platform.controllers();
    }
    if (input.pressed & kButtonB)
        closeOverlay();
}

void App::updateDialog(const InputState& input) {
    if (input.pressed & (kButtonA | kButtonB))
        closeOverlay();
}

bool App::frame() {
    if (!m_platform.beginFrame())
        return false;

    m_now = m_platform.now();
    const float dt = static_cast<float>(std::clamp(m_now - m_last, 0.0, 0.1));
    m_last = m_now;

    if (m_now >= m_statusRefresh) {
        m_status = m_platform.status();
        m_slots = m_platform.controllers();
        m_statusRefresh = m_now + 1.0;
    }

    const InputState input = m_platform.input();
    const uint32_t nav = navigation(input);

    if (m_selected != m_selectedModsFor && m_selected < m_games.size()) {
        m_selectedModsFor = m_selected;
        m_selectedActiveMods = 0;
        const GameEntry& game = m_games[m_selected];
        if (hasMods(game)) {
            ModSet mods;
            const std::string dir =
                ModSet::findDirectory(m_platform.layout().modsDirectory, modDirectoryCandidates(game));
            if (!dir.empty() && mods.load(dir))
                m_selectedActiveMods = mods.activeCount();
        }
    }

    switch (m_screen) {
    case Screen::Shelf:
        switch (m_overlay) {
        case Overlay::None: updateShelf(input, nav); break;
        case Overlay::Options: updateOptions(input, nav); break;
        case Overlay::Controllers: updateControllers(input, nav); break;
        case Overlay::Mods: updateMods(input, nav); break;
        case Overlay::Dialog: updateDialog(input); break;
        }
        break;
    case Screen::Boot:
        m_boot.update(m_now, (input.held & kButtonZR) != 0, m_platform);
        if (m_boot.elapsed(m_now) > 0.6 && (input.pressed & (kButtonA | kButtonB | kButtonPlus)))
            m_boot.skip(m_now);
        break;
    case Screen::Launching:
        break;
    }

    const float target = m_overlay != Overlay::None ? 1.0f : 0.0f;
    m_overlayAnim += (target - m_overlayAnim) * (1.0f - std::exp(-dt * 16.0f));
    if (m_overlay == Overlay::None && m_overlayAnim < 0.01f) {
        m_overlayAnim = 0.0f;
        m_overlayDrawn = Overlay::None;
    }
    m_scroll += (static_cast<float>(m_selected) - m_scroll) * (1.0f - std::exp(-dt * 12.0f));

    m_r.beginFrame(m_platform.framebufferWidth(), m_platform.framebufferHeight(), m_platform.presentFramebuffer(),
                   static_cast<float>(m_now));
    if (m_screen == Screen::Boot) {
        m_boot.draw(m_r, m_now, BootBranding{&m_logo, t(Str::BootPresents)});
    } else {
        drawShelf();
        if (m_screen == Screen::Launching) {
            const float fade = phase(m_now, m_launchStart, 0.3);
            m_r.rect(0, 0, W, H, withAlpha(rgb(0x000000), fade));
        }
    }
    m_r.endFrame();
    m_platform.endFrame();

    if (m_screen == Screen::Boot && m_boot.finished(m_now))
        finishLaunch();
    else if (m_screen == Screen::Launching && m_now - m_launchStart >= 0.3)
        finishLaunch();

    return !m_quit;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

float App::pulse() const { return 0.5f + 0.5f * std::sin(static_cast<float>(m_now) * 4.2f); }

void App::drawEmblem(float cx, float cy, float size, float alpha) {
    // Flat isometric cube: top, left and right faces.
    const float s = size * 0.5f;
    const float h = s * 0.5774f; // tan(30)
    const float top[8] = {cx, cy - s, cx + s * 0.866f, cy - s + h, cx, cy - s + 2 * h, cx - s * 0.866f, cy - s + h};
    const float left[8] = {cx - s * 0.866f, cy - s + h, cx, cy - s + 2 * h, cx, cy + s, cx - s * 0.866f, cy + s - h};
    const float right[8] = {cx, cy - s + 2 * h, cx + s * 0.866f, cy - s + h, cx + s * 0.866f, cy + s - h, cx, cy + s};
    m_r.quad(top, withAlpha(rgb(0xB3A8FF), alpha));
    m_r.quad(left, withAlpha(rgb(0x7062E6), alpha));
    m_r.quad(right, withAlpha(rgb(0x4A3DB8), alpha));
}

void App::drawCubeMark(float cx, float cy, float size, float alpha) {
    if (m_gcLogo)
        m_r.image(m_gcLogo, cx - size * 0.5f, cy - size * 0.5f, size, size, withAlpha(rgb(0xFFFFFF), alpha));
    else
        drawEmblem(cx, cy, size, alpha);
}

float App::drawButtonGlyph(float cx, float cy, const char* glyph, float radius, float alpha) {
    const float size = radius * (std::strlen(glyph) > 1 ? 0.9f : 1.15f);
    const float textWidth = m_r.measure(FontWeight::Bold, size, glyph);
    const float width = std::max(radius * 2.0f, textWidth + radius * 1.1f);
    m_r.roundRect(cx - width * 0.5f, cy - radius, width, radius * 2.0f, radius, withAlpha(rgb(0xF4F2FF), alpha));
    m_r.textMiddle(FontWeight::Bold, size, cx, cy, glyph, withAlpha(kInk, alpha), Align::Center);
    return width;
}

void App::drawHints(const std::vector<Hint>& hints, float right, float y, float alpha) {
    float x = right;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        const char* label = t(it->label);
        const float labelWidth = m_r.measure(FontWeight::Regular, 19.0f, label);
        x -= labelWidth;
        m_r.textMiddle(FontWeight::Regular, 19.0f, x, y, label,
                 withAlpha(kText, alpha));
        const float glyphWidth = std::max(26.0f, m_r.measure(FontWeight::Bold, 12.0f, it->glyph) + 14.0f);
        x -= 9.0f + glyphWidth * 0.5f;
        drawButtonGlyph(x, y, it->glyph, 13.0f, alpha);
        x -= glyphWidth * 0.5f + 28.0f;
    }
}

void App::drawHeader() {
    // PartyBoard wordmark, then the cube and "GAMECUBE" like the console app.
    float x = 40.0f;
    if (m_logo) {
        const float lh = 42.0f;
        const float lw = lh * static_cast<float>(m_logo.width) / static_cast<float>(m_logo.height);
        m_r.image(m_logo, x, 22.0f, lw, lh);
        x += lw + 18.0f;
        m_r.rect(x, 26.0f, 2.0f, 34.0f, withAlpha(kText, 0.25f));
        x += 16.0f;
    }
    drawCubeMark(x + 20.0f, 43.0f, 40.0f, 1.0f);
    x += 50.0f;
    m_r.textMiddle(FontWeight::Display, 26.0f, x, 43.0f, "GAMECUBE", kText, Align::Left, 1.5f);

    // Clock and battery, like the Switch HOME menu.
    x = 1236.0f;
    if (m_status.hour >= 0) {
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%02d:%02d", m_status.hour, m_status.minute);
        x -= m_r.text(FontWeight::Bold, 24.0f, x, 26.0f, clock, kText, Align::Right) + 22.0f;
    }
    if (m_status.battery >= 0) {
        const float bw = 36.0f;
        const float bh = 18.0f;
        const float by = 31.0f;
        x -= bw + 4.0f;
        m_r.roundRectOutline(x, by, bw, bh, 4.0f, 2.0f, withAlpha(kText, 0.9f));
        m_r.roundRect(x + bw + 1.0f, by + 5.0f, 3.0f, bh - 10.0f, 1.5f, withAlpha(kText, 0.9f));
        const float level = std::clamp(m_status.battery / 100.0f, 0.0f, 1.0f);
        const Color fill = m_status.battery <= 15 && !m_status.charging ? kRed : (m_status.charging ? kGreen : kText);
        m_r.roundRect(x + 3.5f, by + 3.5f, (bw - 7.0f) * level, bh - 7.0f, 2.0f, fill);
        char percent[8];
        std::snprintf(percent, sizeof(percent), "%d%%", m_status.battery);
        m_r.text(FontWeight::Regular, 17.0f, x - 8.0f, 30.0f, percent, kTextSoft, Align::Right);
    }
}

void App::drawGeneratedCover(const GameEntry& game, const GameVisual& visual, float x, float y, float w, float h,
                             float s) {
    Color top, bottom;
    switch (game.compatibility) {
    case Compatibility::Supported:
        top = rgb(0x5AA9FF);
        bottom = rgb(0x283CC4);
        break;
    case Compatibility::UnsupportedRegion:
    case Compatibility::UnsupportedRevision:
        top = rgb(0xC77DFF);
        bottom = rgb(0x5B2BB5);
        break;
    case Compatibility::NoSwitchRuntime:
    case Compatibility::OtherGame:
        top = hashedColor(game.disc.gameId, 0.85f);
        bottom = hashedColor(game.disc.gameId, 0.35f);
        break;
    case Compatibility::Unreadable:
        top = rgb(0x6B6790);
        bottom = rgb(0x36334F);
        break;
    }
    const float radius = 14.0f * s;
    m_r.roundRectGradient(x, y, w, h, radius, top, bottom);
    // A soft light from the top-left, like a glossy case insert.
    m_r.softRect(x - w * 0.2f, y - h * 0.15f, w * 0.9f, h * 0.55f, w * 0.4f, w * 0.3f, withAlpha(kText, 0.10f));

    drawCaseBand(x, y, w, s);
    const float band = 30.0f * s;

    const float bw = w - 28.0f * s;
    const float bh = bw / 3.0f;
    const float bx = x + 14.0f * s;
    const float by = y + band + 16.0f * s;
    if (visual.banner) {
        m_r.softRect(bx, by + 4.0f * s, bw, bh, 6.0f * s, 8.0f * s, withAlpha(rgb(0x000000), 0.35f));
        m_r.roundRect(bx - 3.0f * s, by - 3.0f * s, bw + 6.0f * s, bh + 6.0f * s, 8.0f * s, withAlpha(kText, 0.92f));
        m_r.image(visual.banner, bx, by, bw, bh, {}, 6.0f * s);
    } else {
        drawCubeMark(x + w * 0.5f, by + bh * 0.5f, bh * 0.95f, 0.9f);
    }

    const std::vector<std::string> lines = m_r.wrap(FontWeight::Bold, 21.0f * s, game.title(m_language), w - 28.0f * s);
    float ty = by + bh + 16.0f * s;
    for (size_t i = 0; i < lines.size() && i < 3; ++i) {
        const std::string line = m_r.ellipsize(FontWeight::Bold, 21.0f * s, lines[i], w - 24.0f * s);
        m_r.text(FontWeight::Bold, 21.0f * s, x + w * 0.5f + 1.0f, ty + 1.5f, line, withAlpha(rgb(0x000000), 0.35f), Align::Center);
        m_r.text(FontWeight::Bold, 21.0f * s, x + w * 0.5f, ty, line, kText, Align::Center);
        ty += 25.0f * s;
    }

    // A mini-disc peeking out of the case, centred in the space left between
    // the title and the footer.
    const float footerTop = y + h - 36.0f * s;
    const float discR = std::min(w * 0.28f, (footerTop - ty) * 0.5f - 6.0f * s);
    if (discR > 24.0f * s) {
        const float dcx = x + w * 0.5f;
        const float dcy = (ty + footerTop) * 0.5f;
        m_r.circle(dcx, dcy + 3.0f * s, discR, withAlpha(rgb(0x000000), 0.25f));
        m_r.circle(dcx, dcy, discR, rgb(0xE4E1F4));
        m_r.softRect(dcx - discR, dcy - discR, discR * 1.2f, discR * 1.2f, discR * 0.6f, discR * 0.4f,
                     withAlpha(rgb(0xB8F0FF), 0.35f));
        m_r.softRect(dcx - discR * 0.1f, dcy - discR * 0.1f, discR * 1.1f, discR * 1.1f, discR * 0.55f, discR * 0.4f,
                     withAlpha(rgb(0xFFC6F0), 0.3f));
        m_r.ring(dcx, dcy, discR * 0.72f, 1.0f * s, withAlpha(rgb(0x9C97C0), 0.5f));
        m_r.circle(dcx, dcy, discR * 0.36f, rgb(0xC9C5E2));
        m_r.circle(dcx, dcy, discR * 0.14f, bottom);
    }

    drawRegionFooter(game, x, y, w, h, s);
}

void App::drawCaseBand(float x, float y, float w, float s) {
    // GameCube case header strip with the cube mark.
    const float band = 30.0f * s;
    const float radius = 14.0f * s;
    m_r.roundRect(x, y, w, band, radius, rgb(0x16113F));
    m_r.rect(x, y + band * 0.5f, w, band * 0.5f, rgb(0x16113F));
    drawCubeMark(x + 20.0f * s, y + band * 0.5f, 20.0f * s, 1.0f);
    m_r.textMiddle(FontWeight::Display, 14.0f * s, x + 35.0f * s, y + band * 0.5f, "GAMECUBE", kText, Align::Left,
             1.6f * s);
}

void App::drawRegionFooter(const GameEntry& game, float x, float y, float w, float h, float s) {
    const float fy = y + h - 30.0f * s;
    if (game.disc.gameId.empty())
        return;
    const float mid = fy + 8.0f * s;
    m_r.textMiddle(FontWeight::Regular, 13.0f * s, x + 14.0f * s, mid, game.disc.gameId, withAlpha(kText, 0.8f));
    const char* region = t(regionName(game.disc.regionCode()));
    const float rw = m_r.measure(FontWeight::Bold, 12.0f * s, region) + 16.0f * s;
    m_r.roundRect(x + w - 14.0f * s - rw, mid - 10.0f * s, rw, 20.0f * s, 10.0f * s, withAlpha(rgb(0x000000), 0.45f));
    m_r.textMiddle(FontWeight::Bold, 12.0f * s, x + w - 14.0f * s - rw * 0.5f, mid, region, kText, Align::Center);
}

void App::drawBoxArt(const GameVisual& visual, float x, float y, float w, float h, float s) {
    const float radius = 10.0f * s;
    // The spine is drawn foreshortened, as if the case were turned a little.
    const float spineW = visual.spine ? std::round(h * 0.055f) : 0.0f;
    const float frontX = x + spineW;
    const float frontW = w - spineW;
    if (visual.spine) {
        m_r.image(*visual.spine, x, y, spineW + radius, h, rgb(0xB8B4CC), radius);
        m_r.gradientRect(x, y + radius, spineW, h - radius * 2.0f, withAlpha(rgb(0x000000), 0.0f),
                         withAlpha(rgb(0x000000), 0.35f), withAlpha(rgb(0x000000), 0.35f),
                         withAlpha(rgb(0x000000), 0.0f));
    }
    m_r.image(*visual.front, frontX, y, frontW, h, {}, radius);
    // Plastic sleeve: a soft sheen across the top and a crisp crease at the spine.
    m_r.roundRectGradient(frontX, y, frontW, h * 0.45f, radius, withAlpha(rgb(0xFFFFFF), 0.13f),
                          withAlpha(rgb(0xFFFFFF), 0.0f));
    if (visual.spine)
        m_r.rect(frontX - 1.0f, y + radius * 0.5f, 2.0f, h - radius, withAlpha(rgb(0x000000), 0.45f));
}

void App::drawPartyCover(const GameEntry& game, float x, float y, float w, float h, float s) {
    const float radius = 14.0f * s;
    const float band = 30.0f * s;
    m_r.roundRect(x, y, w, h, radius, rgb(0x2B1D5E));

    // The cast from PartyBoard's pre-launch artwork, cropped around the "4".
    const float artH = h - band;
    const float aspect = w / artH;
    const float texAspect = static_cast<float>(m_art.width) / static_cast<float>(m_art.height);
    const float vSpan = 0.88f;
    const float uSpan = vSpan * aspect / texAspect;
    const float u0 = std::clamp(0.665f - uSpan * 0.5f, 0.0f, 1.0f - uSpan);
    const float v0 = std::clamp(0.47f - vSpan * 0.5f, 0.0f, 1.0f - vSpan);
    m_r.image(m_art, x, y + band, w, artH, {}, radius, u0, v0, u0 + uSpan, v0 + vSpan);

    // Wordmark over a dark fade, like a box front.
    m_r.roundRectGradient(x, y + h * 0.52f, w, h * 0.48f, radius, withAlpha(rgb(0x0B0620), 0.0f),
                          withAlpha(rgb(0x0B0620), 0.92f));
    if (m_logo) {
        const float lw = w - 22.0f * s;
        const float lh = lw * static_cast<float>(m_logo.height) / static_cast<float>(m_logo.width);
        m_r.image(m_logo, x + 11.0f * s, y + h - 44.0f * s - lh, lw, lh);
    }
    drawCaseBand(x, y, w, s);
    drawRegionFooter(game, x, y, w, h, s);
}

void App::drawCard(size_t index, float cx, float bottom, float scale, float focus) {
    const GameEntry& game = m_games[index];
    const GameVisual& visual = m_visuals[index];
    const float h = kCardHeight * scale;
    const float w = h * kCardAspect;
    const float x = cx - w * 0.5f;
    const float y = bottom - h;
    const float radius = 14.0f * scale;

    m_r.softRect(x + 8.0f, y + 16.0f, w - 16.0f, h - 6.0f, radius, 24.0f, withAlpha(rgb(0x000000), 0.5f));
    if (focus > 0.01f) {
        m_r.softRect(x - 12.0f, y - 12.0f, w + 24.0f, h + 24.0f, radius + 10.0f, 18.0f,
                     withAlpha(kAccent, 0.32f * focus));
    }

    if (visual.cover)
        m_r.imageCover(visual.cover, x, y, w, h, 0.5f, 0.5f, {}, radius);
    else if (visual.front)
        drawBoxArt(visual, x, y, w, h, scale);
    else if (m_art && isMarioParty4(game))
        drawPartyCover(game, x, y, w, h, scale);
    else
        drawGeneratedCover(game, visual, x, y, w, h, scale);

    if (!game.launchable()) {
        m_r.roundRect(x, y, w, h, radius, withAlpha(rgb(0x0B0820), 0.38f));
        const Color badge = statusColor(game.compatibility);
        m_r.circle(x + w - 22.0f * scale, y + 48.0f * scale, 14.0f * scale, badge);
        m_r.textMiddle(FontWeight::Bold, 18.0f * scale, x + w - 22.0f * scale, y + 48.0f * scale, "!", kInk, Align::Center);
    }
    if (focus < 0.99f)
        m_r.roundRect(x, y, w, h, radius, withAlpha(rgb(0x0B0820), 0.42f * (1.0f - focus)));

    if (focus > 0.01f) {
        const Color ring = mixColor(kAccent, kAccentLight, pulse());
        m_r.roundRectOutline(x - 7.0f, y - 7.0f, w + 14.0f, h + 14.0f, radius + 7.0f, 5.0f, withAlpha(ring, focus));
    }
}

void App::drawInfo() {
    if (m_selected >= m_games.size())
        return;
    const GameEntry& game = m_games[m_selected];
    float x = 120.0f;

    // The disc's own opening.bnr banner, framed like a memory card icon row.
    if (const Texture& banner = m_visuals[m_selected].banner) {
        const float bw = 150.0f;
        const float bh = 50.0f;
        m_r.softRect(x, 504.0f, bw, bh, 8.0f, 10.0f, withAlpha(rgb(0x000000), 0.5f));
        m_r.roundRect(x - 3.0f, 497.0f, bw + 6.0f, bh + 6.0f, 9.0f, withAlpha(kText, 0.92f));
        m_r.image(banner, x, 500.0f, bw, bh, {}, 6.0f);
        x += bw + 24.0f;
    }

    const Color status = statusColor(game.compatibility);
    const char* statusLabel = t(statusText(game.compatibility));
    const float pillWidth = m_r.measure(FontWeight::Bold, 16.0f, statusLabel) + 50.0f;
    const float pillX = 1160.0f - pillWidth;
    m_r.roundRect(pillX, 500.0f, pillWidth, 34.0f, 17.0f, withAlpha(status, 0.18f));
    m_r.roundRectOutline(pillX, 500.0f, pillWidth, 34.0f, 17.0f, 1.5f, withAlpha(status, 0.7f));
    m_r.circle(pillX + 20.0f, 517.0f, 6.0f, status);
    m_r.textMiddle(FontWeight::Bold, 16.0f, pillX + 34.0f, 517.0f, statusLabel, kText);

    // Under the pill: the mods CubeShelf installed for this game, or its runtime.
    if (hasMods(game)) {
        const std::string label = m_selectedActiveMods > 0
                                      ? format(t(Str::ModsActiveCount), static_cast<unsigned>(m_selectedActiveMods))
                                      : std::string(t(Str::ModsNone));
        const float lw = m_r.textMiddle(FontWeight::Regular, 17.0f, 1160.0f, 556.0f, label, kTextSoft, Align::Right);
        drawButtonGlyph(1160.0f - lw - 22.0f, 556.0f, "R", 12.0f, 1.0f);
    } else if (game.catalog && !game.catalog->runtime.empty()) {
        m_r.textMiddle(FontWeight::Regular, 17.0f, 1160.0f, 556.0f, labelled(Str::RuntimeLabel, game.catalog->runtime),
                       kTextSoft, Align::Right);
    }

    const float textWidth = pillX - x - 30.0f;
    m_r.text(FontWeight::Bold, 30.0f, x, 490.0f, m_r.ellipsize(FontWeight::Bold, 30.0f, game.title(m_language), textWidth),
             kText);

    auto joined = [](std::initializer_list<std::string> parts) {
        std::string out;
        for (const std::string& part : parts) {
            if (part.empty())
                continue;
            if (!out.empty())
                out += "  \xC2\xB7  ";
            out += part;
        }
        return out;
    };

    // CubeShelf's game sheet: year, genre, players and runtime from the catalogue.
    float y = 532.0f;
    if (game.catalog) {
        const CatalogEntry& c = *game.catalog;
        const std::string sheet = joined({c.year > 0 ? std::to_string(c.year) : std::string{}, c.genre.get(m_language),
                                          c.players.get(m_language), c.runtime});
        m_r.text(FontWeight::Regular, 18.0f, x, y, m_r.ellipsize(FontWeight::Regular, 18.0f, sheet, textWidth), kTextSoft);
        y += 26.0f;
    }
    // The disc itself.
    std::string disc;
    if (game.error == DiscError::None) {
        disc = joined({game.catalog ? std::string{} : game.maker(m_language), t(regionName(game.disc.regionCode())),
                       format(t(Str::Revision), game.disc.revision), game.disc.gameId,
                       discFormatName(game.disc.format)});
    } else {
        disc = joined({game.fileName, discFormatName(game.disc.format)});
    }
    m_r.text(FontWeight::Regular, game.catalog ? 15.0f : 18.0f, x, y,
             m_r.ellipsize(FontWeight::Regular, 18.0f, disc, textWidth), game.catalog ? kTextDim : kTextSoft);
    y += game.catalog ? 26.0f : 30.0f;

    std::string description = game.description(m_language);
    if (description.empty() && game.error == DiscError::None && !game.disc.hasBanner())
        description = t(Str::BannerMissing);
    std::vector<std::string> lines = m_r.wrap(FontWeight::Regular, 17.0f, description, textWidth);
    if (lines.size() > 2) {
        // Two lines fit above the footer; say there was more.
        lines[1] = m_r.ellipsize(FontWeight::Regular, 17.0f, lines[1] + " " + lines[2] + "\xE2\x80\xA6", textWidth);
        lines.resize(2);
    }
    for (size_t i = 0; i < lines.size(); ++i)
        m_r.text(FontWeight::Regular, 17.0f, x, y + 24.0f * static_cast<float>(i), lines[i], withAlpha(kText, 0.82f));
}

void App::drawEmptyState() {
    const float x = 250.0f, y = 150.0f, w = 780.0f, h = 330.0f;
    m_r.softRect(x, y + 12.0f, w, h, 24.0f, 30.0f, withAlpha(rgb(0x000000), 0.45f));
    m_r.roundRect(x, y, w, h, 24.0f, withAlpha(kPanel, 0.92f));
    m_r.roundRectOutline(x, y, w, h, 24.0f, 1.5f, withAlpha(kTextDim, 0.4f));
    if (m_star) {
        const float bounce = 4.0f * std::sin(static_cast<float>(m_now) * 2.4f);
        m_r.image(m_star, W * 0.5f - 40.0f, y + 26.0f + bounce, 80.0f, 80.0f);
    } else {
        drawEmblem(W * 0.5f, y + 70.0f, 64.0f, 1.0f);
    }
    m_r.text(FontWeight::Display, 32.0f, W * 0.5f, y + 116.0f, t(Str::NoGamesTitle), kText, Align::Center);
    m_r.text(FontWeight::Regular, 19.0f, W * 0.5f, y + 166.0f, t(Str::NoGamesBody), kTextSoft, Align::Center);
    const std::vector<std::string>& dirs = m_platform.layout().gameDirectories;
    const std::string dir = dirs.empty() ? std::string{} : dirs.front();
    const float dw = m_r.measure(FontWeight::Bold, 20.0f, dir) + 40.0f;
    m_r.roundRect(W * 0.5f - dw * 0.5f, y + 200.0f, dw, 42.0f, 21.0f, withAlpha(rgb(0x000000), 0.35f));
    m_r.textMiddle(FontWeight::Bold, 20.0f, W * 0.5f, y + 221.0f, dir,
             kAccentLight, Align::Center);
    m_r.text(FontWeight::Regular, 17.0f, W * 0.5f, y + 266.0f, t(Str::NoGamesFormats), kTextDim, Align::Center);
}

void App::drawFooter() {
    m_r.rect(40.0f, 660.0f, W - 80.0f, 1.0f, withAlpha(kText, 0.16f));

    // Player lamps, lit for each connected controller.
    float x = 44.0f;
    for (int p = 0; p < 4; ++p) {
        const bool on = m_slots.player[p] || (p == 0 && m_slots.handheld);
        char label[8];
        std::snprintf(label, sizeof(label), t(Str::ControllersPlayer), static_cast<unsigned>(p + 1));
        const float lw = 44.0f;
        if (on)
            m_r.roundRect(x, 676.0f, lw, 26.0f, 13.0f, kPlayerColors[p]);
        else
            m_r.roundRectOutline(x, 676.0f, lw, 26.0f, 13.0f, 1.5f, withAlpha(kTextDim, 0.6f));
        m_r.textMiddle(FontWeight::Bold, 14.0f, x + lw * 0.5f, 689.0f, label,
                 on ? kInk : kTextDim, Align::Center);
        x += lw + 8.0f;
    }

    std::vector<Hint> hints;
    if (!m_games.empty())
        hints.push_back({"A", Str::Play});
    hints.push_back({"X", Str::Options});
    hints.push_back({"Y", Str::Controllers});
    if (m_selected < m_games.size() && hasMods(m_games[m_selected]))
        hints.push_back({"R", Str::Mods});
    if (m_games.empty())
        hints.push_back({"-", Str::Refresh});
    hints.push_back({"+", Str::Quit});
    drawHints(hints, 1240.0f, 689.0f);
}

void App::drawBackdrop() {
    if (!m_art) {
        m_r.background(kBgTop, kBgBottom, 1.0f);
        return;
    }
    // PartyBoard's pre-launch artwork, softened behind the shelf and drifting
    // slowly, darkened from the left like the PC screen.
    m_r.rect(0, 0, W, H, rgb(0x120B2E));
    const float drift = 0.5f + 0.5f * std::sin(static_cast<float>(m_now) * 0.07f);
    m_r.imageCover(m_art, -30.0f, -20.0f, W + 60.0f, H + 40.0f, 0.42f + 0.16f * drift, 0.5f, {}, 0.0f, 3.2f);
    const Color clear = withAlpha(rgb(0x07041A), 0.0f);
    const Color shade = rgb(0x07041A);
    m_r.gradientRect(0, 0, W, H, withAlpha(shade, 0.82f), withAlpha(shade, 0.5f), withAlpha(shade, 0.6f),
                     withAlpha(shade, 0.9f));
    m_r.gradientRect(0, 0, W, 110.0f, withAlpha(shade, 0.7f), withAlpha(shade, 0.7f), clear, clear);
    m_r.gradientRect(0, 430.0f, W, H - 430.0f, clear, clear, withAlpha(shade, 0.96f), withAlpha(shade, 0.96f));
}

void App::drawShelf() {
    const float intro = easeOutCubic(phase(m_now, m_shelfFadeStart, 0.6));
    drawBackdrop();
    drawHeader();

    if (m_games.empty()) {
        drawEmptyState();
    } else {
        const float lift = (1.0f - intro) * 40.0f;
        // Far cards first so the selected one overlaps its neighbours.
        std::vector<size_t> order(m_games.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
            return std::fabs(static_cast<float>(a) - m_scroll) > std::fabs(static_cast<float>(b) - m_scroll);
        });
        for (size_t i : order) {
            const float offset = static_cast<float>(i) - m_scroll;
            const float distance = std::fabs(offset);
            const float near = std::min(distance, 1.0f);
            const float travel = distance <= 1.0f ? distance * 248.0f : 248.0f + (distance - 1.0f) * 214.0f;
            const float cx = W * 0.5f + (offset < 0 ? -travel : travel);
            if (cx < -260.0f || cx > W + 260.0f)
                continue;
            const float scale = 1.0f - 0.2f * near;
            const float bob = (1.0f - near) * (10.0f + 2.0f * std::sin(static_cast<float>(m_now) * 2.0f));
            drawCard(i, cx, kShelfBaseline - bob + lift, scale, 1.0f - near);
        }
        drawInfo();
    }
    drawFooter();
    drawToast();

    if (m_overlayDrawn != Overlay::None) {
        const float anim = easeOutCubic(m_overlayAnim);
        switch (m_overlayDrawn) {
        case Overlay::Options: drawOptions(anim); break;
        case Overlay::Controllers: drawControllers(anim); break;
        case Overlay::Mods: drawMods(anim); break;
        case Overlay::Dialog: drawDialog(anim); break;
        case Overlay::None: break;
        }
    }

    if (intro < 1.0f)
        m_r.rect(0, 0, W, H, withAlpha(rgb(0x000000), 1.0f - phase(m_now, m_shelfFadeStart, 0.45)));
}

void App::drawOptions(float anim) {
    m_r.rect(0, 0, W, H, withAlpha(rgb(0x05030F), 0.55f * anim));
    const float pw = 560.0f;
    const float px = W - pw * anim;
    m_r.softRect(px - 30.0f, 0, 60.0f, H, 0.0f, 24.0f, withAlpha(rgb(0x000000), 0.5f * anim));
    m_r.rect(px, 0, pw, H, kPanel);

    m_r.text(FontWeight::Display, 34.0f, px + 44.0f, 36.0f, t(Str::OptionsTitle), kText);

    struct Row {
        Str label;
        Str value;
        Str help;
    };
    const Str aspects[3] = {Str::Aspect43, Str::AspectStretch, Str::AspectWide};
    const Str filters[3] = {Str::FilterNone, Str::FilterSmooth, Str::FilterScanlines};
    const Str languages[3] = {Str::LanguageAuto, Str::LanguageFrench, Str::LanguageEnglish};
    const Row rows[kOptionRows] = {
        {Str::OptBootAnimation, m_settings.bootAnimation ? Str::ValueOnSingular : Str::ValueOffSingular,
         Str::OptBootAnimationHelp},
        {Str::OptAspect, aspects[static_cast<int>(m_settings.aspect)], Str::OptAspectHelp},
        {Str::OptFilter, filters[static_cast<int>(m_settings.filter)], Str::OptFilterHelp},
        {Str::OptLanguage, languages[static_cast<int>(m_settings.language)], Str::OptLanguageHelp},
        {Str::OptRumble, m_settings.rumble ? Str::ValueOn : Str::ValueOff, Str::OptRumbleHelp},
    };

    float y = 108.0f;
    for (int i = 0; i < kOptionRows; ++i) {
        const bool selected = i == m_optionRow;
        const float rx = px + 24.0f;
        const float rw = pw - 48.0f;
        const float rh = 62.0f;
        m_r.roundRect(rx, y, rw, rh, 12.0f, selected ? kPanelRow : withAlpha(kPanelRow, 0.35f));
        if (selected) {
            const Color ring = mixColor(kAccent, kAccentLight, pulse());
            m_r.roundRectOutline(rx - 3.0f, y - 3.0f, rw + 6.0f, rh + 6.0f, 14.0f, 3.0f, ring);
        }
        const float mid = y + rh * 0.5f;
        m_r.textMiddle(FontWeight::Regular, 20.0f, rx + 22.0f, mid,
                 t(rows[i].label), kText);
        const float valueRight = rx + rw - 40.0f;
        const float valueWidth =
            m_r.textMiddle(FontWeight::Bold, 20.0f, valueRight, mid,
                     t(rows[i].value), selected ? kAccentLight : kTextSoft, Align::Right);
        if (selected) {
            const float ax = valueRight + 14.0f;
            m_r.triangle(ax, mid - 7.0f, ax + 8.0f, mid, ax, mid + 7.0f, kAccentLight);
            const float bx = valueRight - valueWidth - 14.0f;
            m_r.triangle(bx, mid - 7.0f, bx, mid + 7.0f, bx - 8.0f, mid, kAccentLight);
        }
        y += rh + 14.0f;
    }

    // Help for the focused row.
    const std::vector<std::string> help =
        m_r.wrap(FontWeight::Regular, 18.0f, t(rows[m_optionRow].help), pw - 100.0f);
    float hy = y + 12.0f;
    for (const std::string& line : help) {
        m_r.text(FontWeight::Regular, 18.0f, px + 44.0f, hy, line, kTextSoft);
        hy += 26.0f;
    }

    // Whether PartyBoard is installed, so a missing engine is visible here too.
    const CatalogEntry* partyboard = nullptr;
    for (const CatalogEntry& entry : m_catalog.games()) {
        if (entry.runtime == "PartyBoard" && !partyboard)
            partyboard = &entry;
    }
    const std::string engine = findEngine(partyboard);
    const std::string engineLine =
        labelled(Str::EngineLabel, engine.empty() ? t(Str::EngineMissing) : t(Str::EngineInstalled));
    m_r.circle(px + 52.0f, 618.0f, 6.0f, engine.empty() ? kOrange : kGreen);
    m_r.textMiddle(FontWeight::Regular, 16.0f, px + 66.0f, 618.0f,
             engineLine, kTextDim);

    m_r.rect(px + 24.0f, 660.0f, pw - 48.0f, 1.0f, withAlpha(kText, 0.16f));
    drawHints({{"A", Str::Change}, {"B", Str::Back}}, px + pw - 40.0f, 689.0f);
}

void App::drawMods(float anim) {
    m_r.rect(0, 0, W, H, withAlpha(rgb(0x05030F), 0.55f * anim));
    const float pw = 620.0f;
    const float px = W - pw * anim;
    m_r.softRect(px - 30.0f, 0, 60.0f, H, 0.0f, 24.0f, withAlpha(rgb(0x000000), 0.5f * anim));
    m_r.rect(px, 0, pw, H, kPanel);

    m_r.text(FontWeight::Display, 34.0f, px + 44.0f, 36.0f, t(Str::ModsTitle), kText);
    if (m_selected < m_games.size())
        m_r.text(FontWeight::Regular, 17.0f, px + 44.0f, 82.0f,
                 m_r.ellipsize(FontWeight::Regular, 17.0f, m_games[m_selected].title(m_language), pw - 88.0f), kTextSoft);

    const std::vector<InstalledMod>& mods = m_mods.mods();
    if (mods.empty()) {
        float y = 150.0f;
        m_r.text(FontWeight::Bold, 21.0f, px + 44.0f, y, t(Str::ModsEmpty), kText);
        y += 44.0f;
        for (const std::string& line : m_r.wrap(FontWeight::Regular, 17.0f, t(Str::ModsCopyHint), pw - 88.0f)) {
            m_r.text(FontWeight::Regular, 17.0f, px + 44.0f, y, line, kTextSoft);
            y += 24.0f;
        }
        const std::string dir = m_r.ellipsize(FontWeight::Bold, 16.0f, m_modsDirectory, pw - 120.0f);
        const float dw = m_r.measure(FontWeight::Bold, 16.0f, dir) + 32.0f;
        m_r.roundRect(px + 44.0f, y + 8.0f, dw, 38.0f, 19.0f, withAlpha(rgb(0x000000), 0.35f));
        m_r.textMiddle(FontWeight::Bold, 16.0f, px + 60.0f, y + 27.0f, dir, kAccentLight);
    } else {
        // A window of rows that follows the focus.
        constexpr int kVisible = 7;
        const float rowH = 62.0f;
        const int count = static_cast<int>(mods.size());
        const int first = std::clamp(m_modRow - kVisible / 2, 0, std::max(0, count - kVisible));
        float y = 118.0f;
        for (int i = first; i < count && i < first + kVisible; ++i) {
            const InstalledMod& mod = mods[static_cast<size_t>(i)];
            const bool selected = i == m_modRow;
            const float rx = px + 24.0f;
            const float rw = pw - 48.0f;
            m_r.roundRect(rx, y, rw, rowH - 8.0f, 12.0f, selected ? kPanelRow : withAlpha(kPanelRow, 0.35f));
            if (selected)
                m_r.roundRectOutline(rx - 3.0f, y - 3.0f, rw + 6.0f, rowH - 2.0f, 14.0f, 3.0f,
                                     mixColor(kAccent, kAccentLight, pulse()));
            const float mid = y + (rowH - 8.0f) * 0.5f;

            // Load order number, then the switch, then the name and its state.
            char order[16];
            std::snprintf(order, sizeof(order), "%d", i + 1);
            m_r.textMiddle(FontWeight::Display, 18.0f, rx + 26.0f, mid, order, kTextDim, Align::Center);
            const bool on = mod.active();
            const float sx = rx + 50.0f;
            m_r.roundRect(sx, mid - 12.0f, 44.0f, 24.0f, 12.0f, on ? kGreen : withAlpha(kTextDim, 0.45f));
            m_r.circle(on ? sx + 32.0f : sx + 12.0f, mid, 9.0f, kText);

            const float nameX = sx + 60.0f;
            const float nameW = rx + rw - nameX - 16.0f;
            const char* state = !mod.present           ? t(Str::ModsMissing)
                                : mod.playerDisabled   ? t(Str::ModsPlayerDisabled)
                                : mod.enabled          ? t(Str::ModsActive)
                                                       : t(Str::ModsInactive);
            const Color stateColor = !mod.present ? kRed : mod.playerDisabled ? kOrange : kTextDim;
            m_r.text(FontWeight::Bold, 18.0f, nameX, mid - 22.0f,
                     m_r.ellipsize(FontWeight::Bold, 18.0f, mod.name, nameW), on ? kText : kTextSoft);
            char detail[96];
            std::snprintf(detail, sizeof(detail), "GameBanana #%d", mod.id);
            m_r.text(FontWeight::Regular, 14.0f, nameX, mid + 2.0f, std::string(state) + "  \xC2\xB7  " + detail,
                     stateColor);
            y += rowH;
        }
        const float hy = 118.0f + rowH * kVisible + 6.0f;
        float ly = hy;
        for (const std::string& line : m_r.wrap(FontWeight::Regular, 16.0f, t(Str::ModsHelp), pw - 88.0f)) {
            m_r.text(FontWeight::Regular, 16.0f, px + 44.0f, ly, line, kTextDim);
            ly += 22.0f;
        }
    }

    m_r.rect(px + 24.0f, 660.0f, pw - 48.0f, 1.0f, withAlpha(kText, 0.16f));
    if (mods.empty())
        drawHints({{"B", Str::Back}}, px + pw - 40.0f, 689.0f);
    else
        drawHints({{"A", Str::ModsToggle}, {"L/R", Str::ModsOrder}, {"B", Str::Back}},
                  px + pw - 40.0f, 689.0f);
}

void App::drawGameCubeController(float cx, float cy, int highlight) {
    const Color body = rgb(0x5A4FCF);
    const Color outline = rgb(0x2E2470);
    const Color recess = rgb(0x372D8F);
    const Color grey = rgb(0xCFCBE6);

    // Shoulders sit behind the body; only their top edge shows.
    m_r.roundRect(cx - 238.0f, cy - 134.0f, 160.0f, 46.0f, 18.0f, rgb(0xB8B3D9));
    m_r.roundRect(cx + 78.0f, cy - 134.0f, 160.0f, 46.0f, 18.0f, rgb(0xB8B3D9));
    m_r.roundRect(cx + 112.0f, cy - 148.0f, 100.0f, 24.0f, 9.0f, rgb(0x8C7CF5));

    // Body: two grips angled outwards and the centre, each drawn over a
    // slightly larger dark copy that becomes the outline.
    const float grip = 0.42f;
    m_r.rotatedRoundRect(cx - 150.0f, cy + 50.0f, 150.0f, 236.0f, 75.0f, grip, outline);
    m_r.rotatedRoundRect(cx + 150.0f, cy + 50.0f, 150.0f, 236.0f, 75.0f, -grip, outline);
    m_r.roundRect(cx - 234.0f, cy - 116.0f, 468.0f, 178.0f, 89.0f, outline);
    m_r.rotatedRoundRect(cx - 150.0f, cy + 50.0f, 142.0f, 228.0f, 71.0f, grip, body);
    m_r.rotatedRoundRect(cx + 150.0f, cy + 50.0f, 142.0f, 228.0f, 71.0f, -grip, body);
    m_r.roundRect(cx - 230.0f, cy - 112.0f, 460.0f, 170.0f, 85.0f, body);
    m_r.softRect(cx - 200.0f, cy - 104.0f, 400.0f, 64.0f, 32.0f, 20.0f, withAlpha(kText, 0.12f));

    // Control stick with its octagonal gate.
    m_r.circle(cx - 152.0f, cy - 22.0f, 47.0f, recess);
    m_r.circle(cx - 152.0f, cy - 22.0f, 32.0f, grey);
    m_r.ring(cx - 152.0f, cy - 22.0f, 22.0f, 3.0f, rgb(0xAFAAD0));

    // D-pad.
    m_r.circle(cx - 78.0f, cy + 70.0f, 32.0f, recess);
    m_r.roundRect(cx - 104.0f, cy + 62.0f, 52.0f, 16.0f, 4.0f, grey);
    m_r.roundRect(cx - 86.0f, cy + 44.0f, 16.0f, 52.0f, 4.0f, grey);

    // C stick.
    m_r.circle(cx + 78.0f, cy + 70.0f, 32.0f, recess);
    m_r.circle(cx + 78.0f, cy + 70.0f, 19.0f, rgb(0xFFD23B));
    m_r.textMiddle(FontWeight::Bold, 15.0f, cx + 78.0f, cy + 70.0f, "C",
             rgb(0x8A6A00), Align::Center);

    // START/PAUSE.
    m_r.circle(cx, cy - 18.0f, 11.0f, grey);

    // Face buttons: big green A, small red B, kidney X and Y.
    m_r.rotatedRoundRect(cx + 206.0f, cy - 34.0f, 24.0f, 62.0f, 12.0f, -0.35f, grey);
    m_r.rotatedRoundRect(cx + 138.0f, cy - 82.0f, 62.0f, 24.0f, 12.0f, -0.2f, grey);
    m_r.circle(cx + 150.0f, cy - 22.0f, 34.0f, rgb(0x2FBF71));
    m_r.circle(cx + 98.0f, cy + 16.0f, 18.0f, rgb(0xE8434B));
    const float letter = 22.0f;
    m_r.textMiddle(FontWeight::Bold, letter, cx + 150.0f, cy - 22.0f, "A",
             rgb(0x0F5F35), Align::Center);
    m_r.textMiddle(FontWeight::Bold, 14.0f, cx + 98.0f, cy + 16.0f, "B",
             rgb(0x7A1218), Align::Center);
    m_r.textMiddle(FontWeight::Bold, 14.0f, cx + 206.0f, cy - 34.0f, "X",
             rgb(0x5A5675), Align::Center);
    m_r.textMiddle(FontWeight::Bold, 14.0f, cx + 138.0f, cy - 82.0f, "Y",
             rgb(0x5A5675), Align::Center);
    m_r.text(FontWeight::Bold, 13.0f, cx - 158.0f, cy - 129.0f, "L", rgb(0x5A5675), Align::Center);
    m_r.text(FontWeight::Bold, 13.0f, cx + 224.0f, cy - 129.0f, "R", rgb(0x5A5675), Align::Center);
    m_r.text(FontWeight::Bold, 13.0f, cx + 162.0f, cy - 145.0f, "Z", kText, Align::Center);

    // Pulsing ring on the control selected in the mapping list.
    struct Spot {
        float x, y, r;
    };
    constexpr Spot spots[11] = {
        {150, -22, 34}, {98, 16, 18},    {206, -34, 30}, {138, -82, 30}, {162, -136, 28}, {-158, -120, 28},
        {158, -120, 28}, {0, -18, 11},   {-152, -22, 47}, {78, 70, 32},  {-78, 70, 32},
    };
    if (highlight >= 0 && highlight < 11) {
        const Spot& s = spots[highlight];
        const Color ring = mixColor(kAccent, kAccentLight, pulse());
        m_r.ring(cx + s.x, cy + s.y, s.r + 9.0f + 3.0f * pulse(), 4.0f, ring);
    }
}

void App::drawControllers(float anim) {
    m_r.rect(0, 0, W, H, withAlpha(rgb(0x05030F), 0.6f * anim));
    const float rise = (1.0f - anim) * 40.0f;
    const float x = 70.0f, y = 50.0f + rise, w = 1140.0f, h = 600.0f;
    m_r.softRect(x, y + 14.0f, w, h, 28.0f, 30.0f, withAlpha(rgb(0x000000), 0.5f * anim));
    m_r.roundRect(x, y, w, h, 28.0f, withAlpha(kPanel, anim));

    m_r.text(FontWeight::Display, 34.0f, x + 48.0f, y + 28.0f, t(Str::ControllersTitle), withAlpha(kText, anim));
    m_r.text(FontWeight::Regular, 18.0f, x + 48.0f, y + 74.0f, t(Str::ControllersSubtitle), withAlpha(kTextSoft, anim));

    drawGameCubeController(x + 330.0f, y + 300.0f, m_mapRow);

    // GameCube control -> Switch control, matching platforms/switch/source/switch_pad.cpp.
    struct Mapping {
        const char* gc;
        Color gcColor;
        const char* sw;
        const char* detail;
    };
    const Mapping maps[11] = {
        {"A", rgb(0x2FBF71), "B", t(Str::MapBottomButton)},
        {"B", rgb(0xE8434B), "A", t(Str::MapRightButton)},
        {"X", rgb(0xCFCBE6), "Y", t(Str::MapLeftButton)},
        {"Y", rgb(0xCFCBE6), "X", t(Str::MapTopButton)},
        {"Z", rgb(0x8C7CF5), "ZR", ""},
        {"L", rgb(0xB8B3D9), "L", t(Str::MapOrZL)},
        {"R", rgb(0xB8B3D9), "R", ""},
        {t(Str::MapStart), rgb(0xCFCBE6), "+", ""},
        {t(Str::MapStick), rgb(0xCFCBE6), "L", t(Str::MapLeftStick)},
        {t(Str::MapCStick), rgb(0xFFD23B), "R", t(Str::MapRightStick)},
        {t(Str::MapDpad), rgb(0xCFCBE6), nullptr, t(Str::MapDirectional)},
    };
    float my = y + 116.0f;
    const float mx = x + 690.0f;
    for (int i = 0; i < 11; ++i) {
        const bool selected = i == m_mapRow;
        const float rowH = 31.0f;
        if (selected)
            m_r.roundRect(mx - 14.0f, my - 2.0f, 420.0f, rowH + 4.0f, 10.0f, withAlpha(kPanelRow, anim));
        const float mid = my + rowH * 0.5f;
        const float chipW = std::max(34.0f, m_r.measure(FontWeight::Bold, 14.0f, maps[i].gc) + 20.0f);
        m_r.roundRect(mx, mid - 13.0f, chipW, 26.0f, 13.0f, withAlpha(maps[i].gcColor, anim));
        m_r.textMiddle(FontWeight::Bold, 14.0f, mx + chipW * 0.5f, mid,
                 maps[i].gc, withAlpha(kInk, anim), Align::Center);
        const float arrowX = mx + 150.0f;
        m_r.rect(arrowX - 18.0f, mid - 1.0f, 18.0f, 2.0f, withAlpha(kTextDim, anim));
        m_r.triangle(arrowX - 26.0f, mid, arrowX - 18.0f, mid - 5.0f, arrowX - 18.0f, mid + 5.0f, withAlpha(kTextDim, anim));
        float glyphW = 26.0f;
        if (maps[i].sw) {
            glyphW = drawButtonGlyph(arrowX + 20.0f, mid, maps[i].sw, 13.0f, anim);
        } else {
            // Directional buttons: a small cross instead of a letter.
            m_r.circle(arrowX + 20.0f, mid, 13.0f, withAlpha(rgb(0xF4F2FF), anim));
            m_r.roundRect(arrowX + 11.0f, mid - 3.0f, 18.0f, 6.0f, 1.5f, withAlpha(kInk, anim));
            m_r.roundRect(arrowX + 17.0f, mid - 9.0f, 6.0f, 18.0f, 1.5f, withAlpha(kInk, anim));
        }
        if (*maps[i].detail) {
            m_r.textMiddle(FontWeight::Regular, 17.0f, arrowX + 30.0f + glyphW * 0.5f + 4.0f, mid, maps[i].detail,
                     withAlpha(selected ? kText : kTextSoft, anim));
        }
        my += rowH + 3.0f;
    }

    // Player slots.
    float sx = x + 48.0f;
    const float sy = y + h - 78.0f;
    for (int p = 0; p < 4; ++p) {
        const bool on = m_slots.player[p] || (p == 0 && m_slots.handheld);
        m_r.roundRect(sx, sy, 140.0f, 52.0f, 14.0f, withAlpha(kPanelRow, anim));
        m_r.circle(sx + 24.0f, sy + 26.0f, 9.0f, withAlpha(on ? kPlayerColors[p] : kTextDim, anim * (on ? 1.0f : 0.5f)));
        char label[8];
        std::snprintf(label, sizeof(label), t(Str::ControllersPlayer), static_cast<unsigned>(p + 1));
        m_r.text(FontWeight::Bold, 17.0f, sx + 42.0f, sy + 6.0f, label, withAlpha(kText, anim));
        const char* state = on ? (p == 0 && m_slots.handheld && !m_slots.player[0] ? t(Str::ControllersHandheld)
                                                                                  : t(Str::ControllersConnected))
                               : t(Str::ControllersEmpty);
        m_r.text(FontWeight::Regular, 13.0f, sx + 42.0f, sy + 28.0f, state, withAlpha(kTextSoft, anim));
        sx += 152.0f;
    }

    drawHints({{"Y", Str::ControllersChangeGrip}, {"B", Str::Back}}, x + w - 40.0f, sy + 26.0f, anim);
}

void App::drawDialog(float anim) {
    m_r.rect(0, 0, W, H, withAlpha(rgb(0x05030F), 0.6f * anim));
    const float w = 660.0f;
    const std::vector<std::string> body = m_r.wrap(FontWeight::Regular, 19.0f, m_dialogBody, w - 96.0f);
    const float h = 170.0f + 28.0f * static_cast<float>(body.size()) + (m_dialogDetail.empty() ? 0.0f : 58.0f);
    const float x = (W - w) * 0.5f;
    const float y = (H - h) * 0.5f + (1.0f - anim) * 30.0f;
    m_r.softRect(x, y + 14.0f, w, h, 24.0f, 30.0f, withAlpha(rgb(0x000000), 0.5f * anim));
    m_r.roundRect(x, y, w, h, 24.0f, withAlpha(kPanel, anim));
    m_r.roundRectOutline(x, y, w, h, 24.0f, 1.5f, withAlpha(kTextDim, 0.4f * anim));

    m_r.text(FontWeight::Bold, 26.0f, x + 48.0f, y + 34.0f, m_dialogTitle, withAlpha(kText, anim));
    float ty = y + 82.0f;
    for (const std::string& line : body) {
        m_r.text(FontWeight::Regular, 19.0f, x + 48.0f, ty, line, withAlpha(kTextSoft, anim));
        ty += 28.0f;
    }
    if (!m_dialogDetail.empty()) {
        const std::string detail = m_r.ellipsize(FontWeight::Bold, 18.0f, m_dialogDetail, w - 130.0f);
        const float dw = m_r.measure(FontWeight::Bold, 18.0f, detail) + 36.0f;
        m_r.roundRect(x + 48.0f, ty + 8.0f, dw, 40.0f, 20.0f, withAlpha(rgb(0x000000), 0.35f * anim));
        m_r.textMiddle(FontWeight::Bold, 18.0f, x + 66.0f, ty + 28.0f, detail,
                 withAlpha(kAccentLight, anim));
    }
    drawHints({{"A", Str::Ok}}, x + w - 44.0f, y + h - 40.0f, anim);
}

void App::drawToast() {
    if (m_toast.empty() || m_now >= m_toastUntil)
        return;
    const float remaining = static_cast<float>(m_toastUntil - m_now);
    const float alpha = std::min(1.0f, remaining / 0.3f) * std::min(1.0f, (2.6f - remaining) / 0.2f);
    const float tw = m_r.measure(FontWeight::Regular, 18.0f, m_toast) + 48.0f;
    const float x = (W - tw) * 0.5f;
    m_r.roundRect(x, 600.0f, tw, 42.0f, 21.0f, withAlpha(rgb(0x0B0820), 0.9f * alpha));
    m_r.roundRectOutline(x, 600.0f, tw, 42.0f, 21.0f, 1.5f, withAlpha(kAccent, 0.6f * alpha));
    m_r.textMiddle(FontWeight::Regular, 18.0f, W * 0.5f, 621.0f, m_toast,
             withAlpha(kText, alpha), Align::Center);
}

} // namespace partyboard::launcher
