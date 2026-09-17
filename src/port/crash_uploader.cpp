#include "port/crash_uploader.h"

#include "port/crash_manifest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

// The default uploader does not upload. It exports: it gathers the files the
// player agreed to send into one folder, and stops there. See
// include/port/crash_uploader.h for why that is the shipped behaviour.

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string readFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void fail(char *error, std::size_t capacity, const char *message)
{
    if (error == nullptr || capacity == 0) return;
    std::snprintf(error, capacity, "%s", message);
}

bool defaultAvailable() { return true; }

const PartyBoardCrashUploader kExportUploader {
    "local folder", defaultAvailable, PartyBoard_CrashExportSubmission};

const PartyBoardCrashUploader *gUploader = &kExportUploader;

} // namespace

extern "C" const PartyBoardCrashUploader *PartyBoard_CrashUploader() { return gUploader; }

extern "C" void PartyBoard_CrashSetUploader(const PartyBoardCrashUploader *uploader)
{
    gUploader = uploader != nullptr ? uploader : &kExportUploader;
}

extern "C" bool PartyBoard_CrashExportSubmission(const char *incidentDirectory, char *error,
    size_t errorCapacity)
{
    if (incidentDirectory == nullptr || incidentDirectory[0] == 0) {
        fail(error, errorCapacity, "no incident directory");
        return false;
    }
    const fs::path incident(incidentDirectory);
    const std::string manifestText = readFile((incident / "manifest.json").string());
    if (manifestText.empty()) {
        fail(error, errorCapacity, "the incident has no manifest");
        return false;
    }
    json manifest = json::parse(manifestText, nullptr, false);
    if (manifest.is_discarded()) {
        fail(error, errorCapacity, "the manifest is not valid JSON");
        return false;
    }

    const bool reportConsent = manifest["consent"].value("report", false);
    const bool dumpConsent = manifest["consent"].value("minidump", false);
    if (!reportConsent && !dumpConsent) {
        // Not an error and not a silent success: there is nothing the player
        // agreed to send, so there is nothing to prepare.
        fail(error, errorCapacity, "the player consented to nothing");
        return false;
    }

    std::error_code code;
    const fs::path submission = incident / "submission";
    fs::remove_all(submission, code);
    fs::create_directories(submission, code);
    if (code) {
        fail(error, errorCapacity, "could not create the submission directory");
        return false;
    }

    // The manifest always goes: it is what a backend reads, and it holds
    // nothing the report does not.
    fs::copy_file(incident / "manifest.json", submission / "manifest.json",
        fs::copy_options::overwrite_existing, code);
    if (code) {
        fail(error, errorCapacity, "could not copy the manifest");
        return false;
    }

    unsigned copied = 0;
    for (const auto &attachment : manifest["attachments"]) {
        const std::string kind = attachment.value("kind", std::string());
        const std::string name = attachment.value("file", std::string());
        if (name.empty()) continue;
        // Each attachment is gated by its own consent. The player who agrees to
        // send the text report has not thereby agreed to send thread stacks.
        if (kind == "report" && !reportConsent) continue;
        if (kind == "minidump" && !dumpConsent) continue;
        if (kind != "report" && kind != "minidump") continue;
        if (!fs::exists(incident / name, code)) continue;
        fs::copy_file(incident / name, submission / name,
            fs::copy_options::overwrite_existing, code);
        if (code) {
            fail(error, errorCapacity, "could not copy an attachment");
            return false;
        }
        ++copied;
    }

    if (copied == 0) {
        fail(error, errorCapacity, "nothing consented to was on disk");
        return false;
    }
    return true;
}

extern "C" unsigned PartyBoard_CrashSubmitReady(void)
{
    // Reads the queue through the manifest module rather than duplicating its
    // file format here, so there is one owner of what the queue means.
    unsigned submitted = 0;
    const PartyBoardCrashUploader *uploader = PartyBoard_CrashUploader();
    if (uploader == nullptr || uploader->available == nullptr || !uploader->available()) {
        return 0;
    }

    std::string queueRoot;
    if (const char *value = std::getenv("PARTYBOARD_CRASH_QUEUE")) {
        queueRoot = value;
    } else if (const char *local = std::getenv("LOCALAPPDATA")) {
        queueRoot = std::string(local) + "\\PartyBoard\\crashes";
    } else {
        return 0;
    }

    const std::string text = readFile(queueRoot + "\\queue.json");
    if (text.empty()) return 0;
    json queue = json::parse(text, nullptr, false);
    if (queue.is_discarded() || !queue.contains("incidents")) return 0;

    for (const auto &incident : queue["incidents"]) {
        if (incident.value("state", std::string()) != "READY") continue;
        const std::string directory = incident.value("directory", std::string());
        if (directory.empty()) continue;
        char error[256] = {};
        const bool ok = uploader->submit != nullptr
            && uploader->submit(directory.c_str(), error, sizeof(error));
        PartyBoard_CrashQueueSetState(directory.c_str(),
            ok ? PARTYBOARD_CRASH_SENT : PARTYBOARD_CRASH_FAILED);
        if (ok) {
            ++submitted;
        } else {
            std::fprintf(stderr, "[crash] submission failed for %s: %s\n", directory.c_str(),
                error[0] != 0 ? error : "unknown");
        }
    }
    return submitted;
}

// ---------------------------------------------------------------------------
// Self-test
//
// The property that matters is the one a player would be angry about getting
// wrong: consenting to the text report must not send the minidump. Every case
// below exists to hold that line, or to check that a refusal is a refusal and
// not a quiet success.
// ---------------------------------------------------------------------------

namespace {

bool expectUpload(bool condition, const char *what)
{
    if (!condition) std::fprintf(stderr, "crash uploader self-test FAILED: %s\n", what);
    return condition;
}

unsigned gFakeSubmissions = 0;
bool gFakeAvailable = true;

bool fakeAvailable() { return gFakeAvailable; }

bool fakeSubmit(const char *incidentDirectory, char *error, size_t errorCapacity)
{
    (void)incidentDirectory;
    (void)error;
    (void)errorCapacity;
    ++gFakeSubmissions;
    return true;
}

const PartyBoardCrashUploader kFakeUploader {"fake", fakeAvailable, fakeSubmit};

void writeText(const fs::path &path, const std::string &text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

// Builds one incident directory with a manifest naming both attachments.
void buildIncident(const fs::path &incident, bool reportConsent, bool dumpConsent)
{
    std::error_code code;
    fs::create_directories(incident, code);
    writeText(incident / "crash-report.txt", "PARTYBOARD_CRASH_REPORT version=1\n");
    writeText(incident / "crash.dmp", "not a real dump");
    json manifest;
    manifest["manifest_version"] = 1;
    manifest["fingerprint"] = "TEST:overlay1:site";
    manifest["attachments"] = json::array({
        json {{"kind", "report"}, {"file", "crash-report.txt"}, {"sensitivity", "normal"}},
        json {{"kind", "minidump"}, {"file", "crash.dmp"}, {"sensitivity", "high"}},
    });
    manifest["consent"] = {{"report", reportConsent}, {"minidump", dumpConsent}, {"asked", true}};
    writeText(incident / "manifest.json", manifest.dump(2));
}

} // namespace

extern "C" bool PartyBoard_CrashUploaderRunSelfTest(void)
{
    bool ok = true;
    std::error_code code;
    const fs::path sandbox = fs::temp_directory_path() / "partyboard-uploader-selftest";
    fs::remove_all(sandbox, code);

    char error[256];

    // 1. Consent to the report only. The dump must stay behind.
    {
        const fs::path incident = sandbox / "report-only";
        buildIncident(incident, true, false);
        error[0] = 0;
        ok &= expectUpload(PartyBoard_CrashExportSubmission(incident.string().c_str(), error,
                               sizeof(error)),
            "exporting a report-only incident failed");
        const fs::path submission = incident / "submission";
        ok &= expectUpload(fs::exists(submission / "manifest.json", code),
            "the manifest was not exported");
        ok &= expectUpload(fs::exists(submission / "crash-report.txt", code),
            "the consented report was not exported");
        ok &= expectUpload(!fs::exists(submission / "crash.dmp", code),
            "the minidump was exported without consent");
    }

    // 2. Consent to both.
    {
        const fs::path incident = sandbox / "both";
        buildIncident(incident, true, true);
        error[0] = 0;
        ok &= expectUpload(PartyBoard_CrashExportSubmission(incident.string().c_str(), error,
                               sizeof(error)),
            "exporting a fully consented incident failed");
        ok &= expectUpload(fs::exists(incident / "submission" / "crash.dmp", code),
            "the consented minidump was not exported");
    }

    // 3. Consent to the dump only. The report must stay behind, which is the
    //    same rule read from the other side.
    {
        const fs::path incident = sandbox / "dump-only";
        buildIncident(incident, false, true);
        error[0] = 0;
        ok &= expectUpload(PartyBoard_CrashExportSubmission(incident.string().c_str(), error,
                               sizeof(error)),
            "exporting a dump-only incident failed");
        ok &= expectUpload(!fs::exists(incident / "submission" / "crash-report.txt", code),
            "the report was exported without consent");
        ok &= expectUpload(fs::exists(incident / "submission" / "crash.dmp", code),
            "the consented minidump was not exported");
    }

    // 4. No consent at all is a refusal with a reason, not a quiet success.
    {
        const fs::path incident = sandbox / "refused";
        buildIncident(incident, false, false);
        error[0] = 0;
        ok &= expectUpload(!PartyBoard_CrashExportSubmission(incident.string().c_str(), error,
                                sizeof(error)),
            "an incident with no consent was exported anyway");
        ok &= expectUpload(error[0] != 0, "a refusal gave no reason");
        ok &= expectUpload(!fs::exists(incident / "submission", code),
            "a refused incident still produced a submission directory");
    }

    // 5. A directory with no manifest is refused, not guessed at.
    {
        const fs::path incident = sandbox / "no-manifest";
        fs::create_directories(incident, code);
        error[0] = 0;
        ok &= expectUpload(!PartyBoard_CrashExportSubmission(incident.string().c_str(), error,
                                sizeof(error)),
            "an incident with no manifest was accepted");
        ok &= expectUpload(error[0] != 0, "the missing manifest gave no reason");
    }
    ok &= expectUpload(!PartyBoard_CrashExportSubmission(nullptr, error, sizeof(error)),
        "a null incident directory was accepted");

    // 6. A registered uploader replaces the default, and clearing restores it.
    {
        const PartyBoardCrashUploader *original = PartyBoard_CrashUploader();
        ok &= expectUpload(original != nullptr, "there was no default uploader");
        PartyBoard_CrashSetUploader(&kFakeUploader);
        ok &= expectUpload(std::strcmp(PartyBoard_CrashUploader()->name, "fake") == 0,
            "the registered uploader was not used");
        PartyBoard_CrashSetUploader(nullptr);
        ok &= expectUpload(PartyBoard_CrashUploader() == original,
            "clearing the uploader did not restore the default");
    }

    // 7. Only READY incidents are submitted, and each ends SENT.
    {
        const std::string previousQueue =
            std::getenv("PARTYBOARD_CRASH_QUEUE") ? std::getenv("PARTYBOARD_CRASH_QUEUE") : "";
        const std::string queue = (sandbox / "queue").string();
#ifdef _WIN32
        _putenv_s("PARTYBOARD_CRASH_QUEUE", queue.c_str());
#else
        setenv("PARTYBOARD_CRASH_QUEUE", queue.c_str(), 1);
#endif
        const fs::path ready = sandbox / "ready";
        const fs::path pending = sandbox / "pending";
        buildIncident(ready, true, false);
        buildIncident(pending, true, false);
        PartyBoard_CrashQueueAdd(ready.string().c_str());
        PartyBoard_CrashQueueAdd(pending.string().c_str());
        PartyBoardCrashConsent accepted {true, false, true};
        PartyBoard_CrashQueueSetConsent(ready.string().c_str(), &accepted, "");

        gFakeSubmissions = 0;
        PartyBoard_CrashSetUploader(&kFakeUploader);
        const unsigned submitted = PartyBoard_CrashSubmitReady();
        PartyBoard_CrashSetUploader(nullptr);
        ok &= expectUpload(submitted == 1, "the number of submissions was not one");
        ok &= expectUpload(gFakeSubmissions == 1, "the uploader was called a different number of times");
        ok &= expectUpload(PartyBoard_CrashQueuePendingCount() == 1,
            "submitting changed the state of an incident nobody had consented to");

        // An unavailable uploader submits nothing rather than failing everything.
        gFakeAvailable = false;
        PartyBoard_CrashSetUploader(&kFakeUploader);
        gFakeSubmissions = 0;
        ok &= expectUpload(PartyBoard_CrashSubmitReady() == 0,
            "an unavailable uploader still submitted");
        ok &= expectUpload(gFakeSubmissions == 0, "an unavailable uploader was still called");
        gFakeAvailable = true;
        PartyBoard_CrashSetUploader(nullptr);

#ifdef _WIN32
        _putenv_s("PARTYBOARD_CRASH_QUEUE", previousQueue.c_str());
#else
        setenv("PARTYBOARD_CRASH_QUEUE", previousQueue.c_str(), 1);
#endif
    }

    fs::remove_all(sandbox, code);
    if (ok) {
        std::printf("Crash uploader: PASS (report-only, dump-only and both consents each export "
                    "exactly what was agreed, no consent and no manifest are refused with a "
                    "reason, a registered uploader replaces and restores the default, only READY "
                    "incidents are submitted, an unavailable uploader submits nothing). Copies "
                    "files into a folder; nothing is sent anywhere.\n");
    }
    return ok;
}
