#include "library.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>

namespace partyboard::launcher {

namespace {

struct KnownTitle {
    const char* id;
    const char* title;
    Compatibility compatibility;
};

// Mirrors src/port/iso_validate.cpp: NTSC-U and PAL boot, NTSC-J is
// recognised so the player is told it is not supported yet.
constexpr KnownTitle kKnownTitles[] = {
    {"GMPE01", "Mario Party 4", Compatibility::Supported},
    {"GMPP01", "Mario Party 4", Compatibility::Supported},
    {"GMPJ01", "Mario Party 4", Compatibility::UnsupportedRegion},
};

const KnownTitle* findKnown(const std::string& id) {
    for (const KnownTitle& known : kKnownTitles) {
        if (id == known.id)
            return &known;
    }
    return nullptr;
}

bool isDirectory(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}

std::string stripExtension(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string lowered(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void collect(const std::string& dir, int depth, std::vector<std::string>& out) {
    DIR* handle = opendir(dir.c_str());
    if (!handle)
        return;
    std::vector<std::string> names;
    while (dirent* entry = readdir(handle)) {
        // Skip dot files, including the "._name" AppleDouble companions macOS
        // writes next to every file it copies onto a FAT SD card.
        if (entry->d_name[0] == '.')
            continue;
        names.emplace_back(entry->d_name);
    }
    closedir(handle);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        const std::string path = joinPath(dir, name);
        if (discFormatFromPath(name) != DiscFormat::Unknown) {
            if (isFile(path))
                out.push_back(path);
        } else if (depth > 0 && isDirectory(path)) {
            collect(path, depth - 1, out);
        }
    }
}

std::string findCover(const std::string& imagePath, const std::string& gameId, const std::string& coversDir) {
    if (!coversDir.empty() && !gameId.empty()) {
        const std::string byId = joinPath(coversDir, gameId + ".png");
        if (isFile(byId))
            return byId;
    }
    const std::string sibling = stripExtension(imagePath) + ".png";
    return isFile(sibling) ? sibling : std::string{};
}

} // namespace

BannerLanguage bannerLanguageFor(Language language) {
    return language == Language::French ? BannerLanguage::French : BannerLanguage::English;
}

Compatibility classifyDisc(const DiscInfo& disc) {
    if (const KnownTitle* known = findKnown(disc.gameId))
        return known->compatibility;
    return Compatibility::OtherGame;
}

std::string GameEntry::title(Language language) const {
    if (error == DiscError::None) {
        if (const BannerText* text = disc.text(bannerLanguageFor(language))) {
            if (!text->longTitle.empty())
                return text->longTitle;
            if (!text->shortTitle.empty())
                return text->shortTitle;
        }
        if (const KnownTitle* known = findKnown(disc.gameId))
            return known->title;
        if (!disc.internalTitle.empty())
            return disc.internalTitle;
    }
    return stripExtension(fileName);
}

std::string GameEntry::maker(Language language) const {
    if (const BannerText* text = disc.text(bannerLanguageFor(language))) {
        if (!text->longMaker.empty())
            return text->longMaker;
        return text->shortMaker;
    }
    return {};
}

std::string GameEntry::description(Language language) const {
    if (const BannerText* text = disc.text(bannerLanguageFor(language)))
        return text->description;
    return {};
}

std::vector<GameEntry> scanLibrary(const std::vector<std::string>& directories,
                                   const std::string& coversDir,
                                   Language language) {
    std::vector<std::string> paths;
    for (const std::string& dir : directories)
        collect(dir, 1, paths);

    std::vector<GameEntry> games;
    games.reserve(paths.size());
    for (const std::string& path : paths) {
        GameEntry entry;
        entry.path = path;
        const size_t slash = path.find_last_of('/');
        entry.fileName = slash == std::string::npos ? path : path.substr(slash + 1);
        entry.error = readDisc(path, entry.disc);
        entry.compatibility = entry.error == DiscError::None ? classifyDisc(entry.disc) : Compatibility::Unreadable;
        entry.coverPath = findCover(path, entry.disc.gameId, coversDir);
        games.push_back(std::move(entry));
    }

    std::stable_sort(games.begin(), games.end(), [language](const GameEntry& a, const GameEntry& b) {
        if (a.compatibility != b.compatibility)
            return a.compatibility < b.compatibility;
        return lowered(a.title(language)) < lowered(b.title(language));
    });
    return games;
}

} // namespace partyboard::launcher
