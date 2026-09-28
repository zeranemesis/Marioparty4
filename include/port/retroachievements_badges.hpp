#pragma once

// Achievement badges, for the achievements window and the unlock notification.
//
// Each badge is downloaded once from the RetroAchievements media server and
// kept under <config>/retroachievements/badges, so later sessions show them
// without the network. Only the unlocked picture is fetched: the locked one is
// the same picture greyed here, as the site greys its own.
//
// RmlUi receives the pictures through an Aurora texture provider
// ("ra-badge://<name>"), not as file paths: RmlUi reads a path that starts
// with '/' as relative to the resources folder, which would break an absolute
// config path on Linux and macOS.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace partyboard::ra::badges {

struct Badge {
    // The server's badge name, such as "123456"; also the file name on disk.
    std::string name;
    std::string url;
};

// The RmlUi image source for a badge, or empty while it is not on disk. A
// window only puts an <img> in for a badge that is there, so RmlUi never asks
// for a picture that does not exist yet.
std::string source(std::string_view name, bool locked);

// Downloads, one at a time on a worker thread, every badge not on disk yet.
// done runs once none is left to fetch, whether or not each one succeeded: at
// once when all are already there, otherwise on the worker thread.
void fetch(std::vector<Badge> badges, std::string userAgent, std::function<void()> done);

} // namespace partyboard::ra::badges
