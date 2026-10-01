#pragma once

// The launcher application: the game shelf, its option and controller
// panels, and the hand-off to the engine through the boot animation.

#include <string>
#include <vector>

#include <map>

#include "boot.hpp"
#include "catalog.hpp"
#include "library.hpp"
#include "mods.hpp"
#include "platform.hpp"
#include "render.hpp"
#include "settings.hpp"

namespace partyboard::launcher {

class App {
public:
    App(Platform& platform, Renderer& renderer);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void init();
    // Updates and draws one frame. Returns false once the launcher should
    // exit: the player quit, or the engine was handed to the loader.
    bool frame();

    size_t gameCount() const { return m_games.size(); }

private:
    enum class Screen : unsigned char { Shelf, Boot, Launching };
    enum class Overlay : unsigned char { None, Options, Controllers, Mods, Dialog };

    struct GameVisual {
        Texture banner;
        Texture cover;              // the player's own PNG, owned here
        const Texture* front = nullptr; // catalogue box art, owned by m_artCache
        const Texture* spine = nullptr;
    };

    struct Hint {
        const char* glyph;
        Str label;
    };

    // State
    void loadArtwork();
    void loadCatalog();
    const Texture* cachedArt(const std::string& resource);
    void rescan(bool announce);
    void releaseTextures();
    void refreshLanguage();
    void openOverlay(Overlay overlay);
    void closeOverlay();
    void showDialog(Str title, Str body, std::string detail);
    void openMods();
    void closeMods();
    std::vector<std::string> modDirectoryCandidates(const GameEntry& game) const;
    bool hasMods(const GameEntry& game) const; // a playable game whose runtime reads CubeShelf mods
    std::string labelled(Str label, const std::string& value) const;
    void showToast(std::string message, double seconds = 2.6);
    void persistSettings();
    std::string findEngine(const CatalogEntry* entry) const;
    void requestLaunch();
    void finishLaunch();

    // Input
    uint32_t navigation(const InputState& input);
    void updateShelf(const InputState& input, uint32_t nav);
    void updateOptions(const InputState& input, uint32_t nav);
    void updateControllers(const InputState& input, uint32_t nav);
    void updateMods(const InputState& input, uint32_t nav);
    void updateDialog(const InputState& input);
    void cycleOption(int row, int delta);

    // Drawing
    void drawShelf();
    void drawBackdrop();
    void drawHeader();
    void drawCard(size_t index, float cx, float bottom, float scale, float focus);
    void drawGeneratedCover(const GameEntry& game, const GameVisual& visual, float x, float y, float w, float h, float s);
    void drawPartyCover(const GameEntry& game, float x, float y, float w, float h, float s);
    void drawBoxArt(const GameVisual& visual, float x, float y, float w, float h, float s);
    void drawCaseBand(float x, float y, float w, float s);
    void drawRegionFooter(const GameEntry& game, float x, float y, float w, float h, float s);
    void drawInfo();
    void drawEmptyState();
    void drawFooter();
    void drawOptions(float anim);
    void drawControllers(float anim);
    void drawMods(float anim);
    void drawDialog(float anim);
    void drawToast();
    void drawGameCubeController(float cx, float cy, int highlight);
    void drawEmblem(float cx, float cy, float size, float alpha);
    // The GameCube cube mark: CubeShelf's logo image, or the drawn emblem.
    void drawCubeMark(float cx, float cy, float size, float alpha);
    float drawButtonGlyph(float cx, float cy, const char* glyph, float radius, float alpha);
    void drawHints(const std::vector<Hint>& hints, float right, float y, float alpha = 1.0f);
    float pulse() const;

    const char* t(Str id) const { return tr(m_language, id); }

    Platform& m_platform;
    Renderer& m_r;

    Settings m_settings;
    Language m_systemLanguage = Language::English;
    Language m_language = Language::English;

    Catalog m_catalog;
    std::vector<GameEntry> m_games;
    std::vector<GameVisual> m_visuals;
    std::map<std::string, Texture> m_artCache; // catalogue covers by resource path

    // PartyBoard artwork shared with the PC pre-launch screen (res/).
    Texture m_logo;       // PartyBoard wordmark
    Texture m_star;       // app icon star
    Texture m_art;        // purple stripes with the Mario Party 4 cast
    Texture m_gcLogo;     // CubeShelf's GameCube cube logo

    // Mods of the selected game, while the Mods panel is open.
    ModSet m_mods;
    int m_modRow = 0;
    bool m_modsDirty = false;
    std::string m_modsDirectory; // where installed.json lives, or where it should go
    size_t m_selectedActiveMods = 0;
    size_t m_selectedModsFor = static_cast<size_t>(-1);

    size_t m_selected = 0;
    float m_scroll = 0.0f;

    Screen m_screen = Screen::Shelf;
    Overlay m_overlay = Overlay::None;
    Overlay m_overlayDrawn = Overlay::None;
    float m_overlayAnim = 0.0f;
    int m_optionRow = 0;
    int m_mapRow = 0;

    std::string m_dialogTitle;
    std::string m_dialogBody;
    std::string m_dialogDetail;
    std::string m_toast;
    double m_toastUntil = 0.0;
    double m_toastDuration = 2.6;
    bool m_appletMode = false;

    BootAnimation m_boot;
    std::vector<std::string> m_launchArgs;
    double m_launchStart = 0.0;

    double m_now = 0.0;
    double m_last = 0.0;
    double m_shelfFadeStart = 0.0;
    double m_statusRefresh = -1.0;
    SystemStatus m_status;
    ControllerSlots m_slots;

    uint32_t m_repeatDir = 0;
    double m_repeatSince = 0.0;
    double m_repeatLast = 0.0;

    bool m_quit = false;
};

} // namespace partyboard::launcher
