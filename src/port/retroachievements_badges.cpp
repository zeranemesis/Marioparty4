#include "port/retroachievements_badges.hpp"

#include "port/http.hpp"
#include "port/main.h"

#include <aurora/lib/logging.hpp>
#include <aurora/rmlui.hpp>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace partyboard::ra::badges {

namespace {

aurora::Module Log("partyboard::ra::badges");

constexpr std::string_view kScheme = "ra-badge";
constexpr std::string_view kPrefix = "ra-badge://";
constexpr std::string_view kLockedSuffix = "/locked";
// A badge is a 64x64 PNG of a few kilobytes; anything far larger is not one.
constexpr size_t kMaxBadgeBytes = 512 * 1024;
// Without a network every request waits out its timeout; after this many in a
// row the rest of the queue is dropped, and the next session tries again.
constexpr int kMaxConsecutiveFailures = 3;

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<std::byte> rgba;
};

// Never destroyed: a worker can still be running while the process exits.
struct State {
    std::mutex mutex;
    std::unordered_set<std::string> onDisk;
    std::deque<Badge> queue;
    std::vector<std::function<void()>> waiting;
    std::string userAgent;
    bool working = false;
    // Decoded pictures by texture source. Never erased, so the span handed to
    // Aurora stays valid after the lock is released.
    std::unordered_map<std::string, Image> decoded;
};

State& state() {
    static auto* instance = new State();
    return *instance;
}

std::filesystem::path directory() { return PartyBoard_ConfigPath / "retroachievements" / "badges"; }

// Badge names are digits. Anything else is refused rather than used as a file
// name.
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
            || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::filesystem::path file_for(std::string_view name) { return directory() / (std::string(name) + ".png"); }

std::optional<Image> decode(const void* data, size_t size) {
    SDL_IOStream* stream = SDL_IOFromConstMem(data, size);
    if (stream == nullptr) {
        return std::nullopt;
    }
    SDL_Surface* loaded = SDL_LoadPNG_IO(stream, true);
    if (loaded == nullptr) {
        return std::nullopt;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (rgba == nullptr) {
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<uint32_t>(rgba->w);
    image.height = static_cast<uint32_t>(rgba->h);
    const size_t rowBytes = static_cast<size_t>(image.width) * 4;
    image.rgba.resize(rowBytes * image.height);
    for (uint32_t row = 0; row < image.height; ++row) {
        std::memcpy(image.rgba.data() + row * rowBytes,
            static_cast<const uint8_t*>(rgba->pixels) + static_cast<size_t>(row) * static_cast<size_t>(rgba->pitch),
            rowBytes);
    }
    SDL_DestroySurface(rgba);
    if (image.width == 0 || image.height == 0) {
        return std::nullopt;
    }
    return image;
}

// The site's locked badges are the same picture, desaturated and darkened.
void grey(Image& image) {
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        const auto r = static_cast<uint32_t>(image.rgba[i]);
        const auto g = static_cast<uint32_t>(image.rgba[i + 1]);
        const auto b = static_cast<uint32_t>(image.rgba[i + 2]);
        const uint32_t luma = (r * 77 + g * 150 + b * 29) >> 8;
        const auto value = static_cast<std::byte>(luma * 7 / 10);
        image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = value;
    }
}

std::optional<std::vector<char>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > kMaxBadgeBytes) {
        return std::nullopt;
    }
    return bytes;
}

// The texture provider behind "ra-badge://<name>" and "ra-badge://<name>/locked".
std::optional<aurora::rmlui::RuntimeTexture> provide(std::string_view source) {
    const std::string key(source);
    auto& s = state();
    const Image* image = nullptr;
    {
        std::lock_guard lock(s.mutex);
        if (const auto it = s.decoded.find(key); it != s.decoded.end()) {
            image = &it->second;
        }
    }
    if (image == nullptr) {
        std::string_view name = source.substr(kPrefix.size());
        const bool locked = name.ends_with(kLockedSuffix);
        if (locked) {
            name.remove_suffix(kLockedSuffix.size());
        }
        std::optional<Image> picture;
        if (valid_name(name)) {
            if (const auto bytes = read_file(file_for(name))) {
                picture = decode(bytes->data(), bytes->size());
            }
        }
        if (picture) {
            if (locked) {
                grey(*picture);
            }
        } else {
            // A transparent pixel rather than nothing: given nothing, Aurora
            // would try the source as a file path and log an error for it.
            Log.warn("Badge {} could not be read", std::string(name));
            picture = Image{1, 1, std::vector<std::byte>(4, std::byte{0})};
        }
        std::lock_guard lock(s.mutex);
        image = &s.decoded.try_emplace(key, std::move(*picture)).first->second;
    }
    return aurora::rmlui::RuntimeTexture{
        .width = image->width,
        .height = image->height,
        .rgba8 = std::span<const std::byte>(image->rgba),
        .premultipliedAlpha = false,
    };
}

// Writes the badge next to its final name first, so a crash midway never
// leaves a truncated picture that a later session would take for a real one.
bool store(const Badge& badge, const std::string& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(directory(), ec);
    const auto path = file_for(badge.name);
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::filesystem::is_regular_file(path, ec);
    }
    return true;
}

void work() {
    auto& s = state();
    int failures = 0;
    for (;;) {
        Badge badge;
        std::string agent;
        {
            std::unique_lock lock(s.mutex);
            if (s.queue.empty() || failures >= kMaxConsecutiveFailures) {
                s.queue.clear();
                s.working = false;
                auto waiting = std::move(s.waiting);
                s.waiting.clear();
                lock.unlock();
                for (auto& done : waiting) {
                    done();
                }
                return;
            }
            badge = std::move(s.queue.front());
            s.queue.pop_front();
            agent = s.userAgent;
        }

        const http::Response response = http::get(badge.url, agent);
        if (response.status == 0) {
            ++failures;
            Log.warn("Badge {} not downloaded: {}", badge.name, response.error);
            continue;
        }
        failures = 0;
        // Decoded once here as a check, so only a picture RmlUi can show is kept.
        if (response.status != 200 || response.body.empty() || response.body.size() > kMaxBadgeBytes
            || !decode(response.body.data(), response.body.size())) {
            Log.warn("Badge {} unusable (HTTP {}, {} bytes)", badge.name, response.status, response.body.size());
            continue;
        }
        if (!store(badge, response.body)) {
            Log.warn("Badge {} could not be written to {}", badge.name, directory().string());
            continue;
        }
        std::lock_guard lock(s.mutex);
        s.onDisk.insert(badge.name);
    }
}

} // namespace

std::string source(std::string_view name, bool locked) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.onDisk.contains(std::string(name))) {
        return {};
    }
    std::string result(kPrefix);
    result += name;
    if (locked) {
        result += kLockedSuffix;
    }
    return result;
}

void fetch(std::vector<Badge> badges, std::string userAgent, std::function<void()> done) {
    static std::once_flag registered;
    std::call_once(registered, [] { aurora::rmlui::register_texture_provider(std::string(kScheme), provide); });

    auto& s = state();
    bool start = false;
    {
        std::lock_guard lock(s.mutex);
        s.userAgent = std::move(userAgent);
        for (auto& badge : badges) {
            if (!valid_name(badge.name) || !badge.url.starts_with("https://") || s.onDisk.contains(badge.name)) {
                continue;
            }
            std::error_code ec;
            if (std::filesystem::is_regular_file(file_for(badge.name), ec)) {
                s.onDisk.insert(badge.name);
                continue;
            }
            bool queued = false;
            for (const auto& other : s.queue) {
                queued = queued || other.name == badge.name;
            }
            if (!queued) {
                s.queue.push_back(std::move(badge));
            }
        }
        if (s.working || !s.queue.empty()) {
            if (done) {
                s.waiting.push_back(std::move(done));
            }
            start = !s.working;
            s.working = true;
        }
    }
    if (start) {
        std::thread(work).detach();
    } else if (done) {
        done();
    }
}

} // namespace partyboard::ra::badges
