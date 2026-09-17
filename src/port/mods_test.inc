// Headless checks for the CubeShelf mod overlay. Exercises list parsing, load
// order and path mapping against a throwaway mod tree; no disc and no Aurora
// FST are needed, so this runs anywhere `partyboard --mods-self-test` runs.

namespace {

unsigned s_modChecks = 0;
bool s_modOk = true;

#define MOD_CHECK(value)                                                                                               \
    do {                                                                                                               \
        ++s_modChecks;                                                                                                 \
        if (!(value)) {                                                                                                \
            s_modOk = false;                                                                                           \
            std::fprintf(stderr, "Mod overlay check failed at line %d: %s\n", __LINE__, #value);                        \
        }                                                                                                              \
    } while (0)

void WriteTestFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

const AuroraOverlayFile* FindOverlay(const std::vector<AuroraOverlayFile>& entries, std::string_view virtualPath) {
    for (const auto& file : entries) {
        if (file.fileName == virtualPath) {
            return &file;
        }
    }
    return nullptr;
}

std::string ReadOverlay(const AuroraOverlayFile& file) {
    void* handle = ModOpen(file.userData);
    if (handle == nullptr) {
        return "<open failed>";
    }
    std::string out(file.size, '\0');
    ModSeek(handle, 0, 0);
    const int64_t read = ModRead(handle, reinterpret_cast<uint8_t*>(out.data()), out.size());
    ModClose(handle);
    out.resize(read < 0 ? 0 : static_cast<size_t>(read));
    return out;
}

} // namespace

extern "C" bool PartyBoard_ModsRunSelfTest(void) {
    s_modChecks = 0;
    s_modOk = true;

    std::error_code error;
    const std::filesystem::path root = std::filesystem::temp_directory_path(error) / "partyboard-mods-self-test";
    if (error) {
        std::fprintf(stderr, "Mod overlay self-test: no temporary directory (%s)\n", error.message().c_str());
        return false;
    }
    std::filesystem::remove_all(root, error);

    // Higher priority pack: replaces a disc file and adds one of its own.
    WriteTestFile(root / "high" / "data" / "board.bin", "HIGH-BOARD");
    WriteTestFile(root / "high" / "data" / "extra.bin", "HIGH-EXTRA");
    WriteTestFile(root / "high" / "cubeshelf-mod.json", "{}");
    // Lower priority pack: claims the same file plus a nested one of its own.
    WriteTestFile(root / "low" / "data" / "board.bin", "LOW-BOARD");
    WriteTestFile(root / "low" / "sound" / "mpgcsnd.msm", "LOW-SOUND");

    const std::filesystem::path listPath = root / "active-mods.txt";
    {
        std::ofstream list(listPath, std::ios::binary);
        list << "\xEF\xBB\xBF";                        // BOM, as some writers emit
        list << (root / "high").string() << "\r\n";    // CRLF and native separators
        list << "\n# commentaire\n";                   // blank line and comment
        list << (root / "low").string() << "\n";
        list << (root / "absent").string() << "\n";    // root the user deleted by hand
    }

    const ModOverlay overlay = BuildOverlay(listPath);
    const std::vector<AuroraOverlayFile> entries = overlay.AuroraFiles();

    MOD_CHECK(overlay.rootCount == 2);
    MOD_CHECK(entries.size() == 3);
    MOD_CHECK(entries.size() == overlay.files.size());
    MOD_CHECK(FindOverlay(entries, "/cubeshelf-mod.json") == nullptr);

    const AuroraOverlayFile* board = FindOverlay(entries, "/data/board.bin");
    MOD_CHECK(board != nullptr);
    if (board != nullptr) {
        MOD_CHECK(board->size == 10);
        MOD_CHECK(ReadOverlay(*board) == "HIGH-BOARD");
    }

    const AuroraOverlayFile* sound = FindOverlay(entries, "/sound/mpgcsnd.msm");
    MOD_CHECK(sound != nullptr);
    if (sound != nullptr) {
        MOD_CHECK(ReadOverlay(*sound) == "LOW-SOUND");
    }

    MOD_CHECK(FindOverlay(entries, "/data/extra.bin") != nullptr);

    for (const auto& file : entries) {
        const std::string_view name(file.fileName);
        MOD_CHECK(name.starts_with("/") && name.find('\\') == std::string_view::npos);
    }

    // Placement of files a pack shipped with no path at all. The rule is driven
    // here with a disc described by hand, because it otherwise needs a mounted
    // disc image and so never ran in CI.
    {
        DiscIndex disc;
        disc.byName["board_e.dat"] = "/mess/board_e.dat";   // one place on the disc
        disc.byName["opening.bnr"] = "/opening.bnr";        // already at the root
        disc.rootFiles.insert("/opening.bnr");
        // "board.bin" occurs twice on the disc, so it is absent from byName.

        ModOverlay placed;
        placed.files.push_back(ModFile{"a", "/board_e.dat", 1});   // placed
        placed.files.push_back(ModFile{"b", "/opening.bnr", 1});   // left: the disc has it here
        placed.files.push_back(ModFile{"c", "/board.bin", 1});     // left: ambiguous
        placed.files.push_back(ModFile{"d", "/stranger.dat", 1});  // left: unknown to the disc
        placed.files.push_back(ModFile{"e", "/data/board.bin", 1});// left: the author gave a path
        PlaceLooseFiles(placed, disc);

        MOD_CHECK(placed.files[0].virtualPath == "/mess/board_e.dat");
        MOD_CHECK(placed.files[1].virtualPath == "/opening.bnr");
        MOD_CHECK(placed.files[2].virtualPath == "/board.bin");
        MOD_CHECK(placed.files[3].virtualPath == "/stranger.dat");
        MOD_CHECK(placed.files[4].virtualPath == "/data/board.bin");

        // A higher priority mod already holding the target keeps it, and the
        // loose file stays where its author put it rather than overwriting.
        ModOverlay contested;
        contested.files.push_back(ModFile{"f", "/mess/board_e.dat", 1});
        contested.files.push_back(ModFile{"g", "/board_e.dat", 1});
        PlaceLooseFiles(contested, disc);
        MOD_CHECK(contested.files[0].virtualPath == "/mess/board_e.dat");
        MOD_CHECK(contested.files[1].virtualPath == "/board_e.dat");
    }

    // An empty list, or one with no usable root, must leave the disc untouched.
    { std::ofstream(listPath, std::ios::binary); }
    const ModOverlay empty = BuildOverlay(listPath);
    MOD_CHECK(empty.rootCount == 0);
    MOD_CHECK(empty.files.empty());
    MOD_CHECK(BuildOverlay({}).files.empty());

    std::filesystem::remove_all(root, error);
    std::fprintf(stderr, "Mod overlay self-test: %u checks, %s\n", s_modChecks, s_modOk ? "passed" : "FAILED");
    return s_modOk;
}

#undef MOD_CHECK
