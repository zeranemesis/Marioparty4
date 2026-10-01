#pragma once

// The launcher application: the game shelf, its option and controller
// panels, and the hand-off to the engine through the boot animation.

#include <string>
#include <vector>

#include "boot.hpp"
#include "library.hpp"
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
    enum class Overlay : unsigned char { None, Options, Controllers, Dialog };

    struct GameVisual {
        Texture banner;
        Texture cover;
    };

    struct Hint {
        const char* glyph;
        Str label;
    };

    // State
    void rescan(bool announce);
    void releaseTextures();
    void refreshLanguage();
    void openOverlay(Overlay overlay);
    void closeOverlay();
    void showDialog(Str title, Str body, std::string detail);
    void showToast(std::string message);
    void persistSettings();
    std::string findEngine() const;
    void requestLaunch();
    void finishLaunch();

    // Input
    uint32_t navigation(const InputState& input);
    void updateShelf(const InputState& input, uint32_t nav);
    void updateOptions(const InputState& input, uint32_t nav);
    void updateControllers(const InputState& input, uint32_t nav);
    void updateDialog(const InputState& input);
    void cycleOption(int row, int delta);

    // Drawing
    void drawShelf();
    void drawHeader();
    void drawCard(size_t index, float cx, float bottom, float scale, float focus);
    void drawGeneratedCover(const GameEntry& game, const GameVisual& visual, float x, float y, float w, float h, float s);
    void drawInfo();
    void drawEmptyState();
    void drawFooter();
    void drawOptions(float anim);
    void drawControllers(float anim);
    void drawDialog(float anim);
    void drawToast();
    void drawGameCubeController(float cx, float cy, int highlight);
    void drawEmblem(float cx, float cy, float size, float alpha);
    float drawButtonGlyph(float cx, float cy, const char* glyph, float radius, float alpha);
    void drawHints(const std::vector<Hint>& hints, float right, float y, float alpha = 1.0f);
    float pulse() const;

    const char* t(Str id) const { return tr(m_language, id); }

    Platform& m_platform;
    Renderer& m_r;

    Settings m_settings;
    Language m_systemLanguage = Language::English;
    Language m_language = Language::English;

    std::vector<GameEntry> m_games;
    std::vector<GameVisual> m_visuals;
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
