#include "port/crash_manifest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
// windows.h defines min and max as macros, which turns every std::min in this
// file into a syntax error.
#define NOMINMAX
#include <windows.h>
#endif

// The tester-facing half of crash reporting. See include/port/crash_manifest.h
// and docs/crash_report_user_pipeline.md.
//
// Nothing in this file opens a socket. It writes files and reads them back.

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxIncidents = 20;
constexpr std::uint64_t kMaxQueueBytes = 200ull * 1024ull * 1024ull;

std::mutex gQueueMutex;

std::string environmentValue(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Windows paths are case-insensitive and mix separators, so both sides are
// normalised before a prefix is compared. The replacement is spliced into the
// ORIGINAL string, so nothing else is case-folded.
std::string normalisedForCompare(std::string text)
{
    std::replace(text.begin(), text.end(), '/', '\\');
    return lowered(std::move(text));
}

struct Replacement {
    std::string from;
    std::string to;
};

// Longest first: LOCALAPPDATA lives under USERPROFILE, and replacing the
// shorter one first would leave the longer one half-rewritten.
std::vector<Replacement> sanitiseRules()
{
    std::vector<Replacement> rules;
    const std::string localAppData = environmentValue("LOCALAPPDATA");
    const std::string appData = environmentValue("APPDATA");
    const std::string profile = environmentValue("USERPROFILE");
    const std::string user = environmentValue("USERNAME");

    std::string install;
#ifdef _WIN32
    char module[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, module, sizeof(module) - 1) > 0) {
        install = fs::path(module).parent_path().string();
    }
#endif

    if (!install.empty()) rules.push_back({install, "<install>"});
    if (!localAppData.empty()) rules.push_back({localAppData, "%LOCALAPPDATA%"});
    if (!appData.empty()) rules.push_back({appData, "%APPDATA%"});
    if (!profile.empty()) rules.push_back({profile, "C:\\Users\\<user>"});

    std::sort(rules.begin(), rules.end(),
        [](const Replacement &a, const Replacement &b) { return a.from.size() > b.from.size(); });

    // Last, and only as a fallback: a bare occurrence of the account name that
    // survived every path rule above.
    if (!user.empty() && user.size() >= 3) rules.push_back({user, "<user>"});
    return rules;
}

std::string sanitise(const std::string &input)
{
    static const std::vector<Replacement> rules = sanitiseRules();
    std::string output = input;
    for (const auto &rule : rules) {
        const std::string needle = normalisedForCompare(rule.from);
        if (needle.empty()) continue;
        for (;;) {
            const std::string haystack = normalisedForCompare(output);
            const auto at = haystack.find(needle);
            if (at == std::string::npos) break;
            output.replace(at, needle.size(), rule.to);
        }
    }
    return output;
}

std::string readFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string matchOne(const std::string &text, const char *pattern, int group = 1)
{
    std::smatch match;
    const std::regex expression(pattern);
    if (std::regex_search(text, match, expression) && match.size() > static_cast<std::size_t>(group)) {
        return match[group].str();
    }
    return {};
}

std::string queueRoot()
{
    std::string base = environmentValue("PARTYBOARD_CRASH_QUEUE");
    if (base.empty()) {
        base = environmentValue("LOCALAPPDATA");
        if (base.empty()) base = ".";
        base += "\\PartyBoard\\crashes";
    }
    return base;
}

std::string queueFile() { return queueRoot() + "\\queue.json"; }

json readQueue()
{
    const std::string text = readFile(queueFile());
    if (text.empty()) return json {{"version", 1}, {"incidents", json::array()}};
    try {
        json parsed = json::parse(text);
        if (!parsed.contains("incidents") || !parsed["incidents"].is_array()) {
            parsed["incidents"] = json::array();
        }
        return parsed;
    } catch (...) {
        // A corrupt queue must not stop the game or lose the directories, which
        // are still on disk and can be re-added.
        return json {{"version", 1}, {"incidents", json::array()}};
    }
}

bool writeQueue(const json &queue)
{
    std::error_code error;
    fs::create_directories(queueRoot(), error);
    std::ofstream file(queueFile(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << queue.dump(2);
    return file.good();
}

const char *stateName(PartyBoardCrashQueueState state)
{
    switch (state) {
    case PARTYBOARD_CRASH_PENDING: return "PENDING";
    case PARTYBOARD_CRASH_DECLINED: return "DECLINED";
    case PARTYBOARD_CRASH_READY: return "READY";
    case PARTYBOARD_CRASH_SENT: return "SENT";
    case PARTYBOARD_CRASH_FAILED: return "FAILED";
    }
    return "PENDING";
}

std::uint64_t directoryBytes(const std::string &directory)
{
    std::uint64_t total = 0;
    std::error_code error;
    for (const auto &entry : fs::recursive_directory_iterator(directory, error)) {
        if (error) break;
        if (entry.is_regular_file(error)) total += entry.file_size(error);
    }
    return total;
}

std::size_t copyOut(const std::string &text, char *output, std::size_t capacity)
{
    if (output == nullptr || capacity == 0) return 0;
    const std::size_t length = std::min(text.size(), capacity - 1);
    std::memcpy(output, text.data(), length);
    output[length] = 0;
    return length;
}

} // namespace

extern "C" size_t PartyBoard_CrashSanitizeText(const char *input, char *output, size_t capacity)
{
    if (input == nullptr) return copyOut("", output, capacity);
    return copyOut(sanitise(input), output, capacity);
}

extern "C" size_t PartyBoard_CrashBuildFingerprint(const char *defectClass, int32_t gameContext,
    const char *symbol, const char *source, const char *module, const char *moduleOffset,
    const char *owningProcess, char *output, size_t capacity)
{
    std::string classText = defectClass != nullptr && defectClass[0] != 0 ? defectClass
                                                                         : "UNKNOWN_FAULT";
    std::string context = "overlay" + std::to_string(gameContext);

    std::string site;
    if (classText == "STACK_OVERFLOW" && owningProcess != nullptr && owningProcess[0] != 0) {
        // The overflowing process names the defect; the faulting symbol is
        // usually whatever unlucky call happened to be on the boundary.
        site = owningProcess;
    } else if (symbol != nullptr && symbol[0] != 0) {
        site = symbol;
        if (source != nullptr && source[0] != 0) {
            // The file and line survive a rebuild; a module offset does not.
            std::string file = source;
            const auto cut = file.find_last_of("/\\");
            if (cut != std::string::npos) file = file.substr(cut + 1);
            site += "@" + file;
        }
    } else {
        site = module != nullptr && module[0] != 0 ? module : "unknown";
        if (moduleOffset != nullptr && moduleOffset[0] != 0) site += std::string("+") + moduleOffset;
    }

    return copyOut(classText + ":" + context + ":" + site, output, capacity);
}

extern "C" bool PartyBoard_CrashWriteManifest(const char *reportPath, const char *minidumpPath,
    const char *directory)
{
    if (reportPath == nullptr) return false;
    const std::string report = readFile(reportPath);
    if (report.empty()) return false;

    const std::string exceptionCode = matchOne(report, "exit_code_hex=(\\S+)");
    const std::string exceptionName = matchOne(report, "exception_name=(\\S+)");
    const std::string operation = matchOne(report, "access_violation operation=(\\w+)");
    const std::string moduleFull = matchOne(report, "faulting_module=([^\\r\\n]+)");
    const std::string moduleOffset = matchOne(report, "faulting_offset=(0x[0-9a-fA-F]+)");
    const std::string terminationReason = matchOne(report, "reason=(\\S+)");
    const std::string owningProcess = matchOne(report, "coroutine stack of ([^:,]+)");
    const std::string frameText = matchOne(report, "simulation_frame=(\\d+)");
    const std::string contextText = matchOne(report, "game_context=(-?\\d+)");
    const std::string overlayText = matchOne(report, "game_context=-?\\d+ overlay=(-?\\d+)");
    const std::string minigameText = matchOne(report, "minigame=(-?\\d+)");
    const std::string revision = matchOne(report, "build_revision=(\\S+)");
    const std::string branch = matchOne(report, "build_branch=(\\S+)");
    const std::string buildType = matchOne(report, "build_type=(\\S+)");
    const std::string buildStamp = matchOne(report, "build_stamp=([^\\r\\n]+)");
    const std::string arch = matchOne(report, "build_arch=(\\S+)");
    const std::string occurredAt = matchOne(report, "report_written_at=([^\\r\\n]+)");
    const std::string topFrame =
        matchOne(report, "FRAME\\s+0\\s+0x[0-9a-f]+\\s+\\S+\\+0x[0-9a-fA-F]+\\s+(\\S+?)\\+0x[0-9a-fA-F]+");
    const std::string topSource = matchOne(report, "FRAME\\s+0[^\\[\\r\\n]*\\[([^\\]]+)\\]");
    const bool audioThread = report.find("salAudioThreadFunc") != std::string::npos;

    std::string defectClass = "UNKNOWN_FAULT";
    if (terminationReason == "COROUTINE_STACK_OVERFLOW") defectClass = "STACK_OVERFLOW";
    else if (terminationReason == "MEMORY_CORRUPTION_DETECTED") defectClass = "MEMORY_CORRUPTION";
    else if (exceptionCode.find("0xC0000374") != std::string::npos) defectClass = "HEAP_CORRUPTION";
    else if (audioThread) defectClass = "AUDIO_FAULT";
    else if (exceptionName == "EXCEPTION_ACCESS_VIOLATION")
        defectClass = operation == "write" ? "ACCESS_VIOLATION_WRITE" : "ACCESS_VIOLATION_READ";
    else if (!exceptionName.empty()) defectClass = exceptionName;

    const std::string moduleName = moduleFull.empty()
        ? std::string("unknown")
        : fs::path(moduleFull).filename().string();

    char fingerprint[512] = {};
    PartyBoard_CrashBuildFingerprint(defectClass.c_str(),
        contextText.empty() ? -1 : std::atoi(contextText.c_str()),
        topFrame.empty() ? nullptr : topFrame.c_str(),
        topSource.empty() ? nullptr : topSource.c_str(), moduleName.c_str(),
        moduleOffset.empty() ? nullptr : moduleOffset.c_str(),
        owningProcess.empty() ? nullptr : owningProcess.c_str(), fingerprint, sizeof(fingerprint));

    const fs::path reportFile(reportPath);
    const std::string incidentDirectory = directory != nullptr && directory[0] != 0
        ? std::string(directory)
        : reportFile.parent_path().string();

    json manifest;
    manifest["manifest_version"] = 1;
    manifest["fingerprint"] = fingerprint;
    manifest["defect_class"] = defectClass;
    manifest["occurred_at"] = sanitise(occurredAt);
    manifest["partyboard_version"] = matchOne(report, "build_describe=(\\S+)");
    manifest["build"] = {{"revision", revision}, {"branch", branch}, {"type", buildType},
        {"stamp", buildStamp}, {"arch", arch}};
#ifdef _WIN32
    manifest["os"] = {{"name", "Windows"}, {"version", ""}, {"locale", ""}};
#else
    manifest["os"] = {{"name", "unknown"}, {"version", ""}, {"locale", ""}};
#endif
    manifest["exception"] = {{"code", exceptionCode}, {"name", exceptionName},
        {"operation", operation}, {"module", moduleName}, {"module_offset", moduleOffset},
        {"symbol", topFrame}, {"source", fs::path(topSource).filename().string()}};
    manifest["game"] = {{"overlay", overlayText.empty() ? -1 : std::atoi(overlayText.c_str())},
        {"game_context", contextText.empty() ? -1 : std::atoi(contextText.c_str())},
        {"minigame", minigameText.empty() ? -1 : std::atoi(minigameText.c_str())},
        {"frame", frameText.empty() ? 0 : std::atoi(frameText.c_str())},
        {"owning_process", owningProcess}};
    manifest["netplay"] = {{"mismatch", matchOne(report, "mismatch=(\\d+)")},
        {"rng_sync", matchOne(report, "rng_sync=(\\d+)")},
        {"repaired", matchOne(report, "repaired=(\\d+)")},
        {"send_errors", matchOne(report, "send_errors=(\\d+)")},
        {"role", matchOne(report, "role=(\\S+)")}};

    // The last few breadcrumbs, sanitised: they are the most useful part of a
    // report for someone who was not there, and the most likely to carry a path.
    json events = json::array();
    {
        const std::regex line("EVENT frame=\\d+ t=\\d+ (\\w+) ([^\\r\\n]+)");
        auto begin = std::sregex_iterator(report.begin(), report.end(), line);
        std::vector<std::string> all;
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            all.push_back(sanitise((*it)[1].str() + " " + (*it)[2].str()));
        }
        const std::size_t keep = std::min<std::size_t>(all.size(), 12);
        for (std::size_t i = all.size() - keep; i < all.size(); ++i) events.push_back(all[i]);
    }
    manifest["events"] = events;

    json attachments = json::array();
    std::error_code error;
    attachments.push_back({{"kind", "report"}, {"file", reportFile.filename().string()},
        {"bytes", static_cast<std::uint64_t>(report.size())}, {"sensitivity", "normal"}});
    if (minidumpPath != nullptr && minidumpPath[0] != 0 && fs::exists(minidumpPath, error)) {
        attachments.push_back({{"kind", "minidump"},
            {"file", fs::path(minidumpPath).filename().string()},
            {"bytes", static_cast<std::uint64_t>(fs::file_size(minidumpPath, error))},
            {"sensitivity", "high"}});
    }
    manifest["attachments"] = attachments;
    manifest["user_note"] = "";
    manifest["consent"] = {{"report", false}, {"minidump", false}, {"asked", false}};

    fs::create_directories(incidentDirectory, error);
    std::ofstream file(incidentDirectory + "/manifest.json", std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << manifest.dump(2);
    return file.good();
}

extern "C" bool PartyBoard_CrashQueueAdd(const char *incidentDirectory)
{
    if (incidentDirectory == nullptr || incidentDirectory[0] == 0) return false;
    std::lock_guard<std::mutex> guard(gQueueMutex);
    json queue = readQueue();
    for (auto &incident : queue["incidents"]) {
        if (incident.value("directory", std::string()) == incidentDirectory) return true;
    }
    queue["incidents"].push_back({{"directory", incidentDirectory},
        {"state", stateName(PARTYBOARD_CRASH_PENDING)}, {"occurrences", 1}});
    return writeQueue(queue);
}

extern "C" bool PartyBoard_CrashQueueSetState(const char *incidentDirectory,
    PartyBoardCrashQueueState state)
{
    if (incidentDirectory == nullptr) return false;
    std::lock_guard<std::mutex> guard(gQueueMutex);
    json queue = readQueue();
    for (auto &incident : queue["incidents"]) {
        if (incident.value("directory", std::string()) != incidentDirectory) continue;
        incident["state"] = stateName(state);
        return writeQueue(queue);
    }
    return false;
}

extern "C" bool PartyBoard_CrashQueueSetConsent(const char *incidentDirectory,
    const PartyBoardCrashConsent *consent, const char *userNote)
{
    if (incidentDirectory == nullptr || consent == nullptr) return false;
    const std::string manifestPath = std::string(incidentDirectory) + "/manifest.json";
    const std::string text = readFile(manifestPath);
    if (text.empty()) return false;
    json manifest;
    try {
        manifest = json::parse(text);
    } catch (...) {
        return false;
    }
    manifest["consent"] = {{"report", consent->report}, {"minidump", consent->minidump},
        {"asked", consent->asked}};
    // A free-text note is the one field a player writes, so it is sanitised like
    // everything else and bounded.
    if (userNote != nullptr) {
        std::string note = sanitise(userNote);
        if (note.size() > 1000) note.resize(1000);
        manifest["user_note"] = note;
    }
    std::ofstream file(manifestPath, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << manifest.dump(2);
    if (!file.good()) return false;

    return PartyBoard_CrashQueueSetState(incidentDirectory,
        consent->report || consent->minidump ? PARTYBOARD_CRASH_READY : PARTYBOARD_CRASH_DECLINED);
}

extern "C" uint32_t PartyBoard_CrashQueuePendingCount(void)
{
    std::lock_guard<std::mutex> guard(gQueueMutex);
    const json queue = readQueue();
    std::uint32_t pending = 0;
    for (const auto &incident : queue["incidents"]) {
        if (incident.value("state", std::string()) == "PENDING") ++pending;
    }
    return pending;
}

extern "C" bool PartyBoard_CrashQueuePrune(void)
{
    std::lock_guard<std::mutex> guard(gQueueMutex);
    json queue = readQueue();
    auto &incidents = queue["incidents"];

    // Oldest SENT first, then oldest DECLINED. A READY incident is never dropped
    // to make room: the player said yes to it, and losing it would quietly undo
    // that.
    const auto droppable = [](const std::string &state) {
        return state == "SENT" ? 2 : state == "DECLINED" ? 1 : 0;
    };

    std::error_code error;
    const auto totalBytes = [&]() {
        std::uint64_t total = 0;
        for (const auto &incident : incidents) {
            const std::string directory = incident.value("directory", std::string());
            if (!directory.empty() && fs::exists(directory, error)) total += directoryBytes(directory);
        }
        return total;
    };

    bool changed = false;
    while (incidents.size() > kMaxIncidents || totalBytes() > kMaxQueueBytes) {
        int bestRank = 0;
        std::size_t bestIndex = incidents.size();
        for (std::size_t index = 0; index < incidents.size(); ++index) {
            const int rank = droppable(incidents[index].value("state", std::string()));
            if (rank > bestRank) {
                bestRank = rank;
                bestIndex = index;
                break; // oldest first: the array is append-ordered
            }
        }
        if (bestIndex >= incidents.size()) break; // nothing droppable left
        const std::string directory = incidents[bestIndex].value("directory", std::string());
        if (!directory.empty()) fs::remove_all(directory, error);
        incidents.erase(incidents.begin() + static_cast<long>(bestIndex));
        changed = true;
    }
    return changed ? writeQueue(queue) : true;
}

// ---------------------------------------------------------------------------
// Startup scan
//
// Deliberately not done while crashing. A dying process has enough to do
// writing its report; building JSON, creating directories and moving files in
// it is how a report gets lost. The next launch has all the time in the world.
// ---------------------------------------------------------------------------

namespace {

// A short, stable name for an incident folder. FNV-1a over the fingerprint,
// which is itself free of pids, addresses and timestamps, so the same defect
// lands in the same folder on every machine and on every launch.
std::string fingerprintFolder(const std::string &fingerprint)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : fingerprint) {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
    return name;
}

std::string manifestFingerprint(const fs::path &manifestPath)
{
    const std::string text = readFile(manifestPath.string());
    if (text.empty()) return {};
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) return {};
    return parsed.value("fingerprint", std::string());
}

// The minidump that belongs to a report: the path the report names, or the
// name the reporter would have used, which is the report's own name with
// "-report" removed and the extension changed.
fs::path minidumpFor(const fs::path &reportPath, const std::string &reportText)
{
    std::error_code error;
    const std::string named = matchOne(reportText, "minidump=([^\\r\\n]+)");
    if (!named.empty() && named.front() != '<') {
        const fs::path beside = reportPath.parent_path() / fs::path(named).filename();
        if (fs::exists(beside, error)) return beside;
        if (fs::exists(named, error)) return named;
    }
    const std::string stem = reportPath.stem().string();
    const std::string prefix = "crash-report-";
    if (stem.rfind(prefix, 0) == 0) {
        const fs::path guess =
            reportPath.parent_path() / ("crash-" + stem.substr(prefix.size()) + ".dmp");
        if (fs::exists(guess, error)) return guess;
    }
    return {};
}

void bumpOccurrence(const fs::path &incident)
{
    const fs::path manifestPath = incident / "manifest.json";
    const std::string text = readFile(manifestPath.string());
    if (text.empty()) return;
    json manifest = json::parse(text, nullptr, false);
    if (manifest.is_discarded()) return;
    manifest["occurrences"] = manifest.value("occurrences", 1u) + 1u;
    const auto now = std::time(nullptr);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", std::gmtime(&now));
    manifest["last_seen"] = stamp;
    std::ofstream file(manifestPath, std::ios::binary | std::ios::trunc);
    if (file) file << manifest.dump(2);
}

} // namespace

extern "C" unsigned PartyBoard_CrashQueueScan(const char *reportsDirectory)
{
    // A supervised session's reports belong to the run directory that produced
    // them. Taking those out of it would remove evidence from a campaign.
    if (const char *supervised = std::getenv("PARTYBOARD_CRASH_DIR")) {
        if (supervised[0] != 0) return 0;
    }
    if (reportsDirectory == nullptr || reportsDirectory[0] == 0) return 0;

    std::error_code error;
    if (!fs::is_directory(reportsDirectory, error)) return 0;

    std::vector<fs::path> reports;
    for (const auto &entry : fs::directory_iterator(reportsDirectory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error)) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("crash-report-", 0) != 0) continue;
        if (entry.path().extension() != ".txt") continue;
        reports.push_back(entry.path());
    }
    // Oldest first, so the folder that survives deduplication holds the first
    // occurrence rather than whichever the filesystem happened to list first.
    std::sort(reports.begin(), reports.end());

    unsigned created = 0;
    for (const auto &report : reports) {
        const std::string text = readFile(report.string());
        if (text.empty()) continue;

        // The manifest has to exist before the fingerprint can be read back, so
        // it is written in place first and moved with the report afterwards.
        if (!PartyBoard_CrashWriteManifest(report.string().c_str(), nullptr,
                report.parent_path().string().c_str())) {
            continue;
        }
        const std::string fingerprint =
            manifestFingerprint(report.parent_path() / "manifest.json");
        fs::remove(report.parent_path() / "manifest.json", error);
        if (fingerprint.empty()) continue;

        const fs::path incident = fs::path(queueRoot()) / fingerprintFolder(fingerprint);
        const bool known = fs::exists(incident / "manifest.json", error);
        if (known) {
            bumpOccurrence(incident);
            // The first occurrence's files stay. This one has been counted, and
            // keeping a hundred identical copies of it helps nobody.
            fs::remove(report, error);
            const fs::path dump = minidumpFor(report, text);
            if (!dump.empty()) fs::remove(dump, error);
            continue;
        }

        fs::create_directories(incident, error);
        if (error) continue;
        const fs::path dump = minidumpFor(report, text);
        const fs::path movedReport = incident / report.filename();
        fs::rename(report, movedReport, error);
        if (error) {
            // Across volumes rename fails; copy then remove.
            error.clear();
            fs::copy_file(report, movedReport, fs::copy_options::overwrite_existing, error);
            if (error) continue;
            fs::remove(report, error);
            error.clear();
        }
        fs::path movedDump;
        if (!dump.empty()) {
            movedDump = incident / dump.filename();
            fs::rename(dump, movedDump, error);
            if (error) {
                error.clear();
                fs::copy_file(dump, movedDump, fs::copy_options::overwrite_existing, error);
                if (error) { movedDump.clear(); error.clear(); }
                else { fs::remove(dump, error); error.clear(); }
            }
        }

        if (!PartyBoard_CrashWriteManifest(movedReport.string().c_str(),
                movedDump.empty() ? nullptr : movedDump.string().c_str(),
                incident.string().c_str())) {
            continue;
        }
        PartyBoard_CrashQueueAdd(incident.string().c_str());
        ++created;
    }

    if (created != 0) PartyBoard_CrashQueuePrune();
    return created;
}

// ---------------------------------------------------------------------------
// Self-test
//
// The two things worth failing on here are privacy and stability. A sanitiser
// is only useful if it can be shown to remove the account name from strings
// that really contain it, and a fingerprint is only useful if two machines
// produce the same one, so it is checked against the values that would betray
// a machine: a pid, an address, a timestamp.
//
// Everything runs in a temporary queue directory, so a test never touches the
// player's real one.
// ---------------------------------------------------------------------------

namespace {

bool expectManifest(bool condition, const char *what)
{
    if (!condition) std::fprintf(stderr, "crash manifest self-test FAILED: %s\n", what);
    return condition;
}

void setEnvironment(const char *name, const std::string &value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

const char *kSyntheticReport =
    "PARTYBOARD_CRASH_REPORT version=1\n"
    "build_describe=0.15.6\n"
    "build_revision=a985b4fca3813f7894a97a005ecbad12fb3fd4ed\n"
    "build_branch=audio-local\n"
    "build_type=RelWithDebInfo\n"
    "build_stamp=Sep 11 2026 11:02:39\n"
    "build_arch=x86_64\n"
    "process_id=46724\n"
    "role=host\n"
    "report_written_at=2026-09-11 13:01:14.982\n"
    "\n[EXCEPTION]\n"
    "exit_code_hex=0xC0000005 exit_code_dec=3221225477\n"
    "exception_name=EXCEPTION_ACCESS_VIOLATION\n"
    "exception_address=0x7ffa21665d14\n"
    "faulting_thread_id=27848\n"
    "faulting_module=PLACEHOLDER_INSTALL\\dol.dll\n"
    "faulting_module_base=0x7ffa21180000 faulting_offset=0x4e5d14\n"
    "access_violation operation=read address=0x2a300e3d5f0\n"
    "\n[SIMULATION]\n"
    "simulation_frame=27744 network_frame=27741\n"
    "game_context=84 overlay=-1 minigame=-1\n"
    "\n[NETWORK]\n"
    "received=27744 rejected=27739 repaired=0 send_errors=0 tx_sequence=87858\n"
    "rng_sync=1 mismatch=0 context_skew=0 local_ready=1 remote_ready=1\n"
    "\n[RECENT EVENTS - newest last]\n"
    "EVENT frame=27740 t=806599734 OVERLAY overlay 84 unloaded\n"
    "EVENT frame=27744 t=806599760 AUDIO sample 1191 of bank 19 freed under voice 12\n"
    "\n[STACK TRACE]\n"
    "FRAME  0 0x7ffa21665d14 dol.dll+0x4e5d14 ensureADPCMBlockDecoded+0xa4 "
    "[PLACEHOLDER_INSTALL\\extern\\musyx\\src\\musyx\\runtime\\hw_pc.c:711]\n"
    "FRAME  7 0x7ffa2166914d dol.dll+0x4e914d salAudioThreadFunc+0xbd "
    "[PLACEHOLDER_INSTALL\\extern\\musyx\\src\\musyx\\runtime\\hw_pc.c:1665]\n";

} // namespace

extern "C" bool PartyBoard_CrashManifestRunSelfTest(void)
{
    bool ok = true;
    std::error_code error;

    const std::string user = environmentValue("USERNAME");
    const std::string profile = environmentValue("USERPROFILE");
    const std::string localAppData = environmentValue("LOCALAPPDATA");

    // ---- sanitisation ----
    char buffer[4096] = {};

    if (!profile.empty()) {
        const std::string personal = profile + "\\Documents\\Mario Party 4.iso";
        PartyBoard_CrashSanitizeText(personal.c_str(), buffer, sizeof(buffer));
        const std::string cleaned = buffer;
        ok &= expectManifest(cleaned.find("<user>") != std::string::npos,
            "a path under the profile was not rewritten");
        if (!user.empty() && user.size() >= 3) {
            ok &= expectManifest(normalisedForCompare(cleaned).find(lowered(user)) == std::string::npos,
                "the account name survived sanitisation of a profile path");
        }
        ok &= expectManifest(cleaned.find("Mario Party 4.iso") != std::string::npos,
            "sanitisation ate the part that was not personal");
    }

    if (!localAppData.empty()) {
        const std::string personal = localAppData + "\\PartyBoard\\crashes";
        PartyBoard_CrashSanitizeText(personal.c_str(), buffer, sizeof(buffer));
        ok &= expectManifest(std::string(buffer).rfind("%LOCALAPPDATA%", 0) == 0,
            "LOCALAPPDATA was not rewritten to its variable");
    }

    if (!user.empty() && user.size() >= 3) {
        const std::string bare = "played by " + user + " on a board";
        PartyBoard_CrashSanitizeText(bare.c_str(), buffer, sizeof(buffer));
        ok &= expectManifest(normalisedForCompare(buffer).find(lowered(user)) == std::string::npos,
            "a bare account name survived sanitisation");
    }

    PartyBoard_CrashSanitizeText("nothing personal here at all", buffer, sizeof(buffer));
    ok &= expectManifest(std::string(buffer) == "nothing personal here at all",
        "sanitisation changed a string with nothing to sanitise");

    PartyBoard_CrashSanitizeText("", buffer, sizeof(buffer));
    ok &= expectManifest(buffer[0] == 0, "sanitising an empty string did not give an empty string");

    // A capacity that cannot hold the result must truncate, not overflow.
    char tiny[8] = {};
    const std::size_t written = PartyBoard_CrashSanitizeText("abcdefghijklmnop", tiny, sizeof(tiny));
    ok &= expectManifest(written == 7 && tiny[7] == 0, "a short buffer was not truncated safely");

    // ---- fingerprints ----
    char fingerprint[256] = {};

    PartyBoard_CrashBuildFingerprint("STACK_OVERFLOW", 92, "memcpy", "string.h:1", "dol.dll",
        "0x1234", "fn_1_30A4", fingerprint, sizeof(fingerprint));
    ok &= expectManifest(std::string(fingerprint) == "STACK_OVERFLOW:overlay92:fn_1_30A4",
        "a stack overflow was not named by its owning process");

    PartyBoard_CrashBuildFingerprint("AUDIO_UAF", 84, "ensureADPCMBlockDecoded",
        "C:\\anything\\hw_pc.c:711", "dol.dll", "0x4e5d14", nullptr, fingerprint,
        sizeof(fingerprint));
    ok &= expectManifest(
        std::string(fingerprint) == "AUDIO_UAF:overlay84:ensureADPCMBlockDecoded@hw_pc.c:711",
        "a symbolised fault did not use its symbol and source");

    PartyBoard_CrashBuildFingerprint("ACCESS_VIOLATION_READ", 3, nullptr, nullptr, "dol.dll",
        "0x26c63b", nullptr, fingerprint, sizeof(fingerprint));
    ok &= expectManifest(
        std::string(fingerprint) == "ACCESS_VIOLATION_READ:overlay3:dol.dll+0x26c63b",
        "an unsymbolised fault did not fall back to module and offset");

    // The same inputs twice must give the same answer, and the answer must not
    // contain anything that differs between two machines running the same build.
    char again[256] = {};
    PartyBoard_CrashBuildFingerprint("ACCESS_VIOLATION_READ", 3, nullptr, nullptr, "dol.dll",
        "0x26c63b", nullptr, again, sizeof(again));
    ok &= expectManifest(std::string(fingerprint) == std::string(again),
        "the fingerprint was not stable across two identical calls");
    ok &= expectManifest(std::string(fingerprint).find("0x7ff") == std::string::npos,
        "the fingerprint carried something that looks like a loaded address");

    // ---- manifest from a report ----
    const fs::path sandbox = fs::temp_directory_path() / "partyboard-manifest-selftest";
    fs::remove_all(sandbox, error);
    fs::create_directories(sandbox / "incident", error);

    std::string report = kSyntheticReport;
    const std::string install = profile.empty() ? std::string("C:\\PartyBoard") : profile;
    for (;;) {
        const auto at = report.find("PLACEHOLDER_INSTALL");
        if (at == std::string::npos) break;
        report.replace(at, std::strlen("PLACEHOLDER_INSTALL"), install);
    }
    const std::string reportPath = (sandbox / "incident" / "crash-report.txt").string();
    {
        std::ofstream file(reportPath, std::ios::binary | std::ios::trunc);
        file << report;
    }

    ok &= expectManifest(PartyBoard_CrashWriteManifest(reportPath.c_str(), nullptr, nullptr),
        "the manifest was not written");

    const std::string manifestText = readFile((sandbox / "incident" / "manifest.json").string());
    ok &= expectManifest(!manifestText.empty(), "the manifest came back empty");
    if (!manifestText.empty()) {
        json manifest = json::parse(manifestText, nullptr, false);
        ok &= expectManifest(!manifest.is_discarded(), "the manifest was not valid JSON");
        if (!manifest.is_discarded()) {
            ok &= expectManifest(manifest.value("defect_class", std::string()) == "AUDIO_FAULT",
                "an audio-thread fault was not classified as one");
            ok &= expectManifest(
                manifest.value("fingerprint", std::string())
                    == "AUDIO_FAULT:overlay84:ensureADPCMBlockDecoded@hw_pc.c:711",
                "the manifest fingerprint was wrong");
            ok &= expectManifest(manifest["game"].value("frame", 0) == 27744,
                "the frame was not carried over");
            ok &= expectManifest(manifest["exception"].value("module", std::string()) == "dol.dll",
                "the module was not reduced to its file name");
            ok &= expectManifest(manifest["consent"].value("report", true) == false
                    && manifest["consent"].value("minidump", true) == false
                    && manifest["consent"].value("asked", true) == false,
                "consent did not start refused on every count");
            ok &= expectManifest(manifest["events"].size() >= 2, "no breadcrumbs were carried over");
        }
        // The whole document, whatever it contains, must not name the account.
        if (!user.empty() && user.size() >= 3) {
            ok &= expectManifest(
                normalisedForCompare(manifestText).find(lowered(user)) == std::string::npos,
                "the account name appears somewhere in the manifest");
        }
        ok &= expectManifest(manifestText.find("46724") == std::string::npos,
            "the process id leaked into the manifest");
    }

    // ---- queue ----
    const std::string previousQueue = environmentValue("PARTYBOARD_CRASH_QUEUE");
    setEnvironment("PARTYBOARD_CRASH_QUEUE", (sandbox / "queue").string());

    const std::string incident = (sandbox / "incident").string();
    ok &= expectManifest(PartyBoard_CrashQueueAdd(incident.c_str()), "the incident was not queued");
    ok &= expectManifest(PartyBoard_CrashQueuePendingCount() == 1,
        "a queued incident was not pending");
    ok &= expectManifest(PartyBoard_CrashQueueAdd(incident.c_str()),
        "queueing the same incident twice failed");
    ok &= expectManifest(PartyBoard_CrashQueuePendingCount() == 1,
        "the same incident was queued twice");

    PartyBoardCrashConsent refused {false, false, true};
    ok &= expectManifest(PartyBoard_CrashQueueSetConsent(incident.c_str(), &refused, "nope"),
        "refusing consent failed");
    ok &= expectManifest(PartyBoard_CrashQueuePendingCount() == 0,
        "a refused incident was still pending");

    PartyBoardCrashConsent accepted {true, false, true};
    ok &= expectManifest(PartyBoard_CrashQueueSetConsent(incident.c_str(), &accepted,
                             "I was buying a star"),
        "accepting consent failed");
    const std::string afterConsent = readFile((sandbox / "incident" / "manifest.json").string());
    ok &= expectManifest(afterConsent.find("I was buying a star") != std::string::npos,
        "the player note was not stored");
    ok &= expectManifest(afterConsent.find("\"minidump\": false") != std::string::npos,
        "consenting to the report also consented to the dump");

    // ---- startup scan ----
    //
    // Two properties matter here: the same defect does not make a second folder,
    // and a supervised session's evidence is never moved out of its run.
    {
        const fs::path loose = sandbox / "loose";
        fs::create_directories(loose, error);
        setEnvironment("PARTYBOARD_CRASH_QUEUE", (sandbox / "scanqueue").string());

        const auto writeReport = [&](const char *name, const char *dumpName, int context) {
            std::string body = report;
            const std::string from = "game_context=84";
            const auto at = body.find(from);
            if (at != std::string::npos) {
                body.replace(at, from.size(), "game_context=" + std::to_string(context));
            }
            {
                std::ofstream file(loose / name, std::ios::binary | std::ios::trunc);
                file << body;
            }
            if (dumpName != nullptr) {
                std::ofstream file(loose / dumpName, std::ios::binary | std::ios::trunc);
                file << "not a real dump";
            }
        };

        // A supervised session must be left alone, whatever is in its directory.
        writeReport("crash-report-peer-0-2026-01-01_000001.txt", "crash-peer-0-2026-01-01_000001.dmp", 84);
        setEnvironment("PARTYBOARD_CRASH_DIR", (sandbox / "somewhere").string());
        ok &= expectManifest(PartyBoard_CrashQueueScan(loose.string().c_str()) == 0,
            "the scan ran inside a supervised session");
        ok &= expectManifest(fs::exists(loose / "crash-report-peer-0-2026-01-01_000001.txt", error),
            "the scan moved a supervised session's report");
        setEnvironment("PARTYBOARD_CRASH_DIR", "");

        // One report becomes one incident, and the files move with it.
        ok &= expectManifest(PartyBoard_CrashQueueScan(loose.string().c_str()) == 1,
            "the first report did not create exactly one incident");
        ok &= expectManifest(!fs::exists(loose / "crash-report-peer-0-2026-01-01_000001.txt", error),
            "the report was not moved out of the loose directory");
        ok &= expectManifest(!fs::exists(loose / "crash-peer-0-2026-01-01_000001.dmp", error),
            "the minidump was not moved with its report");
        ok &= expectManifest(PartyBoard_CrashQueuePendingCount() == 1,
            "the new incident was not pending");

        // The same defect again: counted, not duplicated.
        writeReport("crash-report-peer-0-2026-01-01_000002.txt", nullptr, 84);
        ok &= expectManifest(PartyBoard_CrashQueueScan(loose.string().c_str()) == 0,
            "the same fingerprint created a second incident");
        ok &= expectManifest(PartyBoard_CrashQueuePendingCount() == 1,
            "the duplicate was queued as its own incident");
        ok &= expectManifest(!fs::exists(loose / "crash-report-peer-0-2026-01-01_000002.txt", error),
            "the counted duplicate was left lying around");

        // A different overlay is a different fingerprint, so a different folder.
        writeReport("crash-report-peer-0-2026-01-01_000003.txt", nullptr, 92);
        ok &= expectManifest(PartyBoard_CrashQueueScan(loose.string().c_str()) == 1,
            "a different fingerprint did not create its own incident");
        ok &= expectManifest(PartyBoard_CrashQueuePendingCount() == 2,
            "the second defect was not pending");

        // Nothing left to do is not an error.
        ok &= expectManifest(PartyBoard_CrashQueueScan(loose.string().c_str()) == 0,
            "an empty scan reported work it did not do");
        ok &= expectManifest(PartyBoard_CrashQueueScan(nullptr) == 0,
            "scanning nowhere reported work");
    }

    setEnvironment("PARTYBOARD_CRASH_QUEUE", previousQueue);
    fs::remove_all(sandbox, error);

    if (ok) {
        std::printf("Crash manifest: PASS (profile/LOCALAPPDATA/bare-name sanitisation, short "
                    "buffer, three fingerprint shapes and their stability, manifest from a report "
                    "with no account name and no pid, queue add/dedup/consent/refusal, startup "
                    "scan creating one incident per fingerprint, counting a repeat instead of "
                    "duplicating it, and refusing to touch a supervised session). "
                    "Files only; nothing is sent anywhere.\n");
    }
    return ok;
}
