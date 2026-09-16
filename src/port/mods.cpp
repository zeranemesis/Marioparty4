// PartyBoard <-> CubeShelf mod overlay.
//
// CubeShelf writes the enabled mod content roots, highest priority first, to
// the file named by PARTYBOARD_MOD_LIST. Every file below a root is registered
// with Aurora's DVD overlay at the matching virtual disc path, so DVDOpen(),
// DVDConvertPathToEntrynum() and DVDFastOpen() all resolve to the modded file
// instead of the one stored in the disc image. Nothing on disc is rewritten, so
// enabling, disabling and reordering mods only costs a relaunch.

#include "port/mods.h"

#include <aurora/dvd.h>
#include <aurora/lib/logging.hpp>
#include <dolphin/dvd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

aurora::Module PartyBoardModsLog("partyboard::mods");

// A mod pack larger than this is treated as broken rather than spending
// minutes walking a runaway directory tree.
constexpr size_t k_maxOverlayFiles = 65536;

// Launcher metadata, never part of the virtual disc.
constexpr std::string_view k_manifestName = "cubeshelf-mod.json";

struct ModFile {
    std::filesystem::path hostPath;
    std::string virtualPath;
    size_t size = 0;
};

struct ModOverlay {
    // A deque keeps element addresses stable across the move into s_overlay,
    // which matters because Aurora keeps the pointers below for as long as the
    // overlay is installed.
    std::deque<ModFile> files;
    int rootCount = 0;

    // Call only once the overlay sits in its final home: every entry points
    // into `files`.
    std::vector<AuroraOverlayFile> AuroraFiles() const {
        std::vector<AuroraOverlayFile> entries;
        entries.reserve(files.size());
        for (const ModFile& file : files) {
            entries.push_back(AuroraOverlayFile{file.virtualPath.c_str(),
                                                const_cast<ModFile*>(&file), file.size});
        }
        return entries;
    }
};

ModOverlay s_overlay;

std::string AsciiLower(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    });
    return lowered;
}

std::string_view Trim(std::string_view value) {
    const auto isBlank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && isBlank(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isBlank(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

std::string Utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

std::filesystem::path FromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::filesystem::path ModListPath() {
#ifdef _WIN32
    if (const wchar_t* path = _wgetenv(L"PARTYBOARD_MOD_LIST"); path != nullptr && *path != 0) {
        return std::filesystem::path(path);
    }
#else
    if (const char* path = std::getenv("PARTYBOARD_MOD_LIST"); path != nullptr && *path != 0) {
        return std::filesystem::path(path);
    }
#endif
    return {};
}

std::vector<std::filesystem::path> ReadModRoots(const std::filesystem::path& listPath) {
    std::vector<std::filesystem::path> roots;
    std::ifstream list(listPath);
    if (!list.is_open()) {
        PartyBoardModsLog.warn("Could not open mod list {}", Utf8(listPath));
        return roots;
    }

    const std::filesystem::path listDirectory = listPath.parent_path();
    std::string line;
    bool first = true;
    while (std::getline(list, line)) {
        std::string_view entry = line;
        if (first) {
            first = false;
            // Strip a UTF-8 BOM if the launcher wrote one.
            if (entry.starts_with("\xEF\xBB\xBF")) {
                entry.remove_prefix(3);
            }
        }
        entry = Trim(entry);
        if (entry.empty() || entry.front() == '#') {
            continue;
        }

        std::filesystem::path root = FromUtf8(entry);
        if (root.is_relative() && !listDirectory.empty()) {
            root = listDirectory / root;
        }

        std::error_code error;
        root = std::filesystem::weakly_canonical(root, error);
        if (error) {
            PartyBoardModsLog.warn("Ignoring unusable mod root {}: {}", entry, error.message());
            continue;
        }
        if (!std::filesystem::is_directory(root, error)) {
            PartyBoardModsLog.warn("Ignoring missing mod root {}", Utf8(root));
            continue;
        }
        roots.push_back(std::move(root));
    }
    return roots;
}

// Collects every file below `root` as an overlay entry. Roots are visited in
// priority order and the first mod claiming a path keeps it, which mirrors the
// ordering CubeShelf shows in its conflict report.
void CollectRoot(const std::filesystem::path& root, ModOverlay& overlay, std::unordered_set<std::string>& claimed) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
        PartyBoardModsLog.warn("Could not read mod root {}: {}", Utf8(root), error.message());
        return;
    }

    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            PartyBoardModsLog.warn("Stopped reading {}: {}", Utf8(root), error.message());
            return;
        }
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }

        const std::filesystem::path hostPath = iterator->path();
        if (AsciiLower(Utf8(hostPath.filename())) == k_manifestName) {
            continue;
        }

        const uintmax_t size = std::filesystem::file_size(hostPath, error);
        if (error) {
            PartyBoardModsLog.warn("Skipping unreadable {}: {}", Utf8(hostPath), error.message());
            error.clear();
            continue;
        }
        if (size > std::numeric_limits<uint32_t>::max()) {
            PartyBoardModsLog.warn("Skipping {}: files above 4 GiB cannot be overlaid", Utf8(hostPath));
            continue;
        }

        const std::filesystem::path relative = std::filesystem::relative(hostPath, root, error);
        if (error) {
            error.clear();
            continue;
        }
        std::string virtualPath = "/" + Utf8(relative);
        std::replace(virtualPath.begin(), virtualPath.end(), '\\', '/');

        if (!claimed.insert(AsciiLower(virtualPath)).second) {
            // A higher priority mod already owns this path.
            continue;
        }
        if (overlay.files.size() >= k_maxOverlayFiles) {
            PartyBoardModsLog.error("Mod overlay stopped at {} files, the rest is ignored", k_maxOverlayFiles);
            return;
        }

        overlay.files.push_back(ModFile{hostPath, std::move(virtualPath), static_cast<size_t>(size)});
    }
}

// Records where each file name occurs on the disc. A name seen twice is marked
// ambiguous and is never placed automatically.
void IndexDiscNames(const std::string& directory, int depth,
                    std::unordered_map<std::string, std::string>& byName,
                    std::unordered_set<std::string>& ambiguous, int& budget) {
    constexpr int k_maxDepth = 8;

    DVDDir dir{};
    if (!DVDOpenDir(directory.c_str(), &dir)) {
        return;
    }

    DVDDirEntry entry{};
    while (budget > 0 && DVDReadDir(&dir, &entry)) {
        if (entry.name == nullptr) {
            continue;
        }
        --budget;
        const std::string child = directory == "/" ? "/" + std::string(entry.name)
                                                   : directory + "/" + entry.name;
        if (entry.isDir) {
            if (depth < k_maxDepth) {
                IndexDiscNames(child, depth + 1, byName, ambiguous, budget);
            }
            continue;
        }
        const std::string name = AsciiLower(entry.name);
        if (ambiguous.contains(name)) {
            continue;
        }
        if (const auto existing = byName.find(name); existing != byName.end()) {
            byName.erase(existing);
            ambiguous.insert(name);
            continue;
        }
        byName.emplace(name, child);
    }

    DVDCloseDir(&dir);
}

// A pack that ships its files loose, with no folders at all, says nothing about
// where they belong, so they land at the root of the disc where nothing reads
// them - the shape of the first real pack this was tried against, whose
// board_e.dat belongs under mess/. When the disc carries exactly one file of
// that name, that is where it goes. A name the disc does not carry, carries
// twice, or already carries at its root is left exactly where the author put it.
void PlaceLooseFiles(ModOverlay& overlay) {
    std::vector<ModFile*> loose;
    std::unordered_set<std::string> taken;
    for (ModFile& file : overlay.files) {
        taken.insert(AsciiLower(file.virtualPath));
        const bool atDiscRoot = file.virtualPath.find('/', 1) == std::string::npos;
        if (atDiscRoot && DVDConvertPathToEntrynum(file.virtualPath.c_str()) < 0) {
            loose.push_back(&file);
        }
    }
    if (loose.empty()) {
        return;
    }

    std::unordered_map<std::string, std::string> byName;
    std::unordered_set<std::string> ambiguous;
    int budget = 8192;
    IndexDiscNames("/", 0, byName, ambiguous, budget);

    for (ModFile* file : loose) {
        const std::string name = AsciiLower(file->virtualPath.substr(1));
        const auto match = byName.find(name);
        if (match == byName.end()) {
            PartyBoardModsLog.warn("{} matches no file on the disc, leaving it at the root",
                                   file->virtualPath);
            continue;
        }
        if (!taken.insert(AsciiLower(match->second)).second) {
            PartyBoardModsLog.warn("{} belongs at {}, already claimed by a higher priority mod",
                                   file->virtualPath, match->second);
            continue;
        }
        PartyBoardModsLog.info("{} shipped without a path, placing it at {}",
                               file->virtualPath, match->second);
        file->virtualPath = match->second;
    }
}

ModOverlay BuildOverlay(const std::filesystem::path& listPath) {
    ModOverlay overlay;
    if (listPath.empty()) {
        return overlay;
    }

    PartyBoardModsLog.info("Reading CubeShelf mod list {}", Utf8(listPath));
    const std::vector<std::filesystem::path> roots = ReadModRoots(listPath);
    overlay.rootCount = static_cast<int>(roots.size());

    std::unordered_set<std::string> claimed;
    for (size_t i = 0; i < roots.size(); i++) {
        const size_t before = overlay.files.size();
        CollectRoot(roots[i], overlay, claimed);
        PartyBoardModsLog.info("Mod root {} ({}) contributes {} file(s)", i, Utf8(roots[i]),
                               overlay.files.size() - before);
    }
    return overlay;
}

void* ModOpen(void* userdata) {
    const auto* file = static_cast<const ModFile*>(userdata);
    if (file == nullptr) {
        return nullptr;
    }
#ifdef _WIN32
    FILE* handle = _wfopen(file->hostPath.c_str(), L"rb");
#else
    FILE* handle = std::fopen(file->hostPath.c_str(), "rb");
#endif
    if (handle == nullptr) {
        PartyBoardModsLog.error("Could not open mod file {}", Utf8(file->hostPath));
    }
    return handle;
}

void ModClose(void* handle) {
    if (handle != nullptr) {
        std::fclose(static_cast<FILE*>(handle));
    }
}

int64_t ModRead(void* handle, uint8_t* buf, size_t len) {
    if (handle == nullptr || (buf == nullptr && len != 0)) {
        return -1;
    }
    auto* file = static_cast<FILE*>(handle);
    const size_t read = std::fread(buf, 1, len, file);
    if (read != len && std::ferror(file) != 0) {
        return -1;
    }
    return static_cast<int64_t>(read);
}

int64_t ModSeek(void* handle, int64_t offset, int32_t whence) {
    if (handle == nullptr) {
        return -1;
    }
    auto* file = static_cast<FILE*>(handle);
    const int origin = whence == 1 ? SEEK_CUR : (whence == 2 ? SEEK_END : SEEK_SET);
#ifdef _WIN32
    if (_fseeki64(file, offset, origin) != 0) {
        return -1;
    }
    return _ftelli64(file);
#else
    if (std::fseek(file, static_cast<long>(offset), origin) != 0) {
        return -1;
    }
    return static_cast<int64_t>(std::ftell(file));
#endif
}

} // namespace

extern "C" int PartyBoard_InitMods(void) {
    s_overlay = BuildOverlay(ModListPath());

    if (s_overlay.rootCount == 0) {
        PartyBoardModsLog.info("No mod enabled, booting the original disc");
        return 0;
    }
    if (s_overlay.files.empty()) {
        PartyBoardModsLog.warn("Every mod root is empty, booting the original disc");
        return 0;
    }

    // Needs the disc's own FST, so it happens here rather than in BuildOverlay,
    // which the self-test drives with no disc open.
    PlaceLooseFiles(s_overlay);

    const AuroraOverlayCallbacks callbacks{ModOpen, ModClose, ModRead, ModSeek};
    aurora_dvd_overlay_callbacks(&callbacks);

    const std::vector<AuroraOverlayFile> entries = s_overlay.AuroraFiles();
    std::vector<s32> entryNums(entries.size(), -1);
    aurora_dvd_overlay_files(entries.data(), entries.size(), entryNums.data());

    int applied = 0;
    for (size_t i = 0; i < entryNums.size(); i++) {
        if (entryNums[i] < 0) {
            PartyBoardModsLog.error("Aurora rejected the overlay {}", s_overlay.files[i].virtualPath);
            continue;
        }
        PartyBoardModsLog.debug("Overlay {} -> {}", s_overlay.files[i].virtualPath, Utf8(s_overlay.files[i].hostPath));
        applied++;
    }

    PartyBoardModsLog.info("{} file(s) overlaid from {} mod root(s)", applied, s_overlay.rootCount);
    return applied;
}

extern "C" int PartyBoard_GetModRootCount(void) { return s_overlay.rootCount; }

#include "mods_test.inc"
