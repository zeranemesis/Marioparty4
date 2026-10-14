// SDL3 on libnx, exercised the way Aurora and PartyBoard use it: a window,
// gamepads (with their names and types), events, base/pref paths and file
// I/O through SDL_IOStream, plus an audio stream and the 2D renderer to show
// the results on screen.
//
// Hold A for a 440 Hz tone, press + to quit. Everything shown is also
// printed to stdout (nxlink).

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;

struct Tone {
    bool on = false;
    double phase = 0.0;
};

void SDLCALL feedAudio(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* tone = static_cast<Tone*>(user);
    const int frames = additional / int(sizeof(float) * 2);
    if (frames <= 0)
        return;
    std::vector<float> samples(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = tone->on ? 0.2f * float(std::sin(tone->phase)) : 0.0f;
        tone->phase = std::fmod(tone->phase + kTwoPi * 440.0 / 48000.0, kTwoPi);
        samples[size_t(i) * 2] = v;
        samples[size_t(i) * 2 + 1] = v;
    }
    SDL_PutAudioStreamData(stream, samples.data(), int(samples.size() * sizeof(float)));
}

// Writes then reads back a file in the pref path, through SDL_IOStream.
bool fileRoundTrip(const char* prefPath) {
    if (!prefPath)
        return false;
    const std::string path = std::string(prefPath) + "sdl3-probe.txt";
    static const char kText[] = "PartyBoard SDL3 probe";
    SDL_IOStream* out = SDL_IOFromFile(path.c_str(), "wb");
    if (!out)
        return false;
    const bool wrote = SDL_WriteIO(out, kText, sizeof(kText)) == sizeof(kText);
    SDL_CloseIO(out);
    char back[sizeof(kText)] = {};
    SDL_IOStream* in = SDL_IOFromFile(path.c_str(), "rb");
    if (!in)
        return false;
    const bool read = SDL_ReadIO(in, back, sizeof(back)) == sizeof(back);
    SDL_CloseIO(in);
    SDL_RemovePath(path.c_str());
    return wrote && read && SDL_memcmp(back, kText, sizeof(kText)) == 0;
}

std::string pressedButtons(SDL_Gamepad* pad) {
    std::string out;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const auto button = static_cast<SDL_GamepadButton>(b);
        if (SDL_GetGamepadButton(pad, button)) {
            const char* name = SDL_GetGamepadStringForButton(button);
            out += out.empty() ? "" : " ";
            out += name ? name : "?";
        }
    }
    return out.empty() ? "-" : out;
}

} // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("PartyBoard SDL3 probe", 1280, 720, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::printf("window/renderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderScale(renderer, 2.0f, 2.0f);

    Tone tone;
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
    SDL_AudioStream* audio =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feedAudio, &tone);
    if (audio)
        SDL_ResumeAudioStreamDevice(audio);

    const int version = SDL_GetVersion();
    char* prefPath = SDL_GetPrefPath("MarioPartyRD", "PartyBoard");
    const char* basePath = SDL_GetBasePath();
    const bool fileOk = fileRoundTrip(prefPath);

    std::vector<std::string> header;
    char line[256];
    std::snprintf(line, sizeof(line), "PartyBoard SDL3 probe - SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(version),
                  SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version));
    header.push_back(line);
    std::snprintf(line, sizeof(line), "video: %s   renderer: %s", SDL_GetCurrentVideoDriver(),
                  SDL_GetRendererName(renderer));
    header.push_back(line);
    std::snprintf(line, sizeof(line), "audio: %s   %s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none",
                  audio ? "stream open" : SDL_GetError());
    header.push_back(line);
    std::snprintf(line, sizeof(line), "base: %s", basePath ? basePath : "(none)");
    header.push_back(line);
    std::snprintf(line, sizeof(line), "pref: %s   file I/O: %s", prefPath ? prefPath : "(none)", fileOk ? "OK" : "FAILED");
    header.push_back(line);
    for (const std::string& h : header)
        std::printf("%s\n", h.c_str());

    std::vector<SDL_Gamepad*> pads;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which)) {
                    pads.push_back(pad);
                    std::printf("gamepad added: %s\n", SDL_GetGamepadName(pad));
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (size_t i = 0; i < pads.size(); ++i) {
                    if (SDL_GetGamepadID(pads[i]) == event.gdevice.which) {
                        SDL_CloseGamepad(pads[i]);
                        pads.erase(pads.begin() + long(i));
                        break;
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START)
                    running = false;
                break;
            default:
                break;
            }
        }

        tone.on = false;
        for (SDL_Gamepad* pad : pads)
            tone.on = tone.on || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);

        SDL_SetRenderDrawColor(renderer, tone.on ? 40 : 20, 22, tone.on ? 30 : 48, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 235, 235, 245, 255);
        float y = 12.0f;
        for (const std::string& h : header) {
            SDL_RenderDebugText(renderer, 12.0f, y, h.c_str());
            y += 12.0f;
        }
        y += 8.0f;
        std::snprintf(line, sizeof(line), "gamepads: %zu", pads.size());
        SDL_RenderDebugText(renderer, 12.0f, y, line);
        y += 12.0f;
        for (SDL_Gamepad* pad : pads) {
            const auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0f; };
            std::snprintf(line, sizeof(line), "%s (type %d)", SDL_GetGamepadName(pad), int(SDL_GetGamepadType(pad)));
            SDL_RenderDebugText(renderer, 20.0f, y, line);
            y += 12.0f;
            std::snprintf(line, sizeof(line), "  L %+.2f %+.2f  R %+.2f %+.2f  buttons: %s", axis(SDL_GAMEPAD_AXIS_LEFTX),
                          axis(SDL_GAMEPAD_AXIS_LEFTY), axis(SDL_GAMEPAD_AXIS_RIGHTX), axis(SDL_GAMEPAD_AXIS_RIGHTY),
                          pressedButtons(pad).c_str());
            SDL_RenderDebugText(renderer, 20.0f, y, line);
            y += 16.0f;
        }
        SDL_RenderDebugText(renderer, 12.0f, 340.0f, "Hold A for a 440 Hz tone.  + quits.");
        SDL_RenderPresent(renderer);
    }

    for (SDL_Gamepad* pad : pads)
        SDL_CloseGamepad(pad);
    if (audio)
        SDL_DestroyAudioStream(audio);
    SDL_free(prefPath);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
