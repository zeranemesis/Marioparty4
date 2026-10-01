#include "settings.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>

namespace partyboard::launcher {

namespace {

const char* aspectKey(AspectMode mode) {
    switch (mode) {
    case AspectMode::Stretch169: return "stretch";
    case AspectMode::Wide169: return "wide";
    case AspectMode::Original43: break;
    }
    return "4:3";
}

const char* filterKey(ScreenFilter filter) {
    switch (filter) {
    case ScreenFilter::Smooth: return "smooth";
    case ScreenFilter::Scanlines: return "crt";
    case ScreenFilter::None: break;
    }
    return "none";
}

const char* languageKey(LanguagePref language) {
    switch (language) {
    case LanguagePref::French: return "fr";
    case LanguagePref::English: return "en";
    case LanguagePref::Auto: break;
    }
    return "auto";
}

std::string trim(const std::string& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t'))
        ++start;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n'))
        --end;
    return s.substr(start, end - start);
}

bool parseBool(const std::string& value, bool fallback) {
    if (value == "on" || value == "true" || value == "1")
        return true;
    if (value == "off" || value == "false" || value == "0")
        return false;
    return fallback;
}

} // namespace

Language Settings::resolveLanguage(Language system) const {
    switch (language) {
    case LanguagePref::French: return Language::French;
    case LanguagePref::English: return Language::English;
    case LanguagePref::Auto: break;
    }
    return system;
}

bool loadSettings(const std::string& path, Settings& out) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return false;

    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[')
            continue;
        const size_t eq = text.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(text.substr(0, eq));
        const std::string value = trim(text.substr(eq + 1));

        if (key == "boot_animation") {
            out.bootAnimation = parseBool(value, out.bootAnimation);
        } else if (key == "aspect") {
            if (value == "stretch")
                out.aspect = AspectMode::Stretch169;
            else if (value == "wide")
                out.aspect = AspectMode::Wide169;
            else if (value == "4:3")
                out.aspect = AspectMode::Original43;
        } else if (key == "filter") {
            if (value == "smooth")
                out.filter = ScreenFilter::Smooth;
            else if (value == "crt")
                out.filter = ScreenFilter::Scanlines;
            else if (value == "none")
                out.filter = ScreenFilter::None;
        } else if (key == "language") {
            if (value == "fr")
                out.language = LanguagePref::French;
            else if (value == "en")
                out.language = LanguagePref::English;
            else if (value == "auto")
                out.language = LanguagePref::Auto;
        } else if (key == "rumble") {
            out.rumble = parseBool(value, out.rumble);
        } else if (key == "last_game") {
            out.lastGame = value;
        } else if (key == "engine") {
            out.enginePath = value;
        }
    }
    std::fclose(file);
    return true;
}

bool saveSettings(const std::string& path, const Settings& settings) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
        makeDirectories(path.substr(0, slash));

    FILE* file = std::fopen(path.c_str(), "w");
    if (!file)
        return false;
    std::fprintf(file, "# PartyBoard GameCube launcher\n");
    std::fprintf(file, "boot_animation=%s\n", settings.bootAnimation ? "on" : "off");
    std::fprintf(file, "aspect=%s\n", aspectKey(settings.aspect));
    std::fprintf(file, "filter=%s\n", filterKey(settings.filter));
    std::fprintf(file, "language=%s\n", languageKey(settings.language));
    std::fprintf(file, "rumble=%s\n", settings.rumble ? "on" : "off");
    if (!settings.lastGame.empty())
        std::fprintf(file, "last_game=%s\n", settings.lastGame.c_str());
    if (!settings.enginePath.empty())
        std::fprintf(file, "engine=%s\n", settings.enginePath.c_str());
    return std::fclose(file) == 0;
}

std::vector<std::string> buildLaunchArgs(const std::string& enginePath,
                                         const std::string& discPath,
                                         const std::string& launcherPath,
                                         const Settings& settings,
                                         Language language,
                                         const std::string& modList) {
    std::vector<std::string> args;
    args.push_back(enginePath);
    args.push_back("--disc-image=" + discPath);
    args.push_back(std::string("--aspect=") + aspectKey(settings.aspect));
    args.push_back(std::string("--filter=") + filterKey(settings.filter));
    args.push_back(std::string("--lang=") + (language == Language::French ? "fr" : "en"));
    args.push_back(std::string("--rumble=") + (settings.rumble ? "on" : "off"));
    if (!modList.empty())
        args.push_back("--mod-list=" + modList);
    if (!launcherPath.empty())
        args.push_back("--launcher=" + launcherPath);
    return args;
}

std::string joinArgv(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& arg : args) {
        if (!out.empty())
            out.push_back(' ');
        const bool quote = arg.empty() || arg.find_first_of(" \t") != std::string::npos;
        if (quote)
            out.push_back('"');
        out += arg;
        if (quote)
            out.push_back('"');
    }
    return out;
}

bool makeDirectories(const std::string& path) {
    if (path.empty())
        return false;
    // Never try to create the device root ("sdmc:" or "/").
    const size_t device = path.find(":/");
    size_t pos = device == std::string::npos ? 1 : device + 2;
    while (pos <= path.size()) {
        const size_t next = path.find('/', pos);
        const std::string partial = path.substr(0, next == std::string::npos ? path.size() : next);
        if (!partial.empty() && mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST)
            return false;
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return true;
}

} // namespace partyboard::launcher
