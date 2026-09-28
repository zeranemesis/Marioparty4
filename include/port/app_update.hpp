#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Updates of the Android app straight from GitHub, like PartyBoardOnline.exe's UpdateService on
// Windows. Every build CI publishes (.github/workflows/build.yml, platforms/android/scripts/
// publish-update.sh) lands in the release "partyboard-android-latest" with a manifest,
// android-update.json, that names the build, its APK, the APK's SHA-256 and the changes since the
// previous published build. The app reads that manifest, and when it names a newer build it opens
// the APK's download in the browser; Android installs it over the app, keeping the saves, and
// only accepts it when it is signed with the installed app's key. The app asks for no install
// permission of its own: Play Protect blocks unknown apps that do.
namespace partyboard::update {

inline constexpr std::string_view kManifestUrl =
    "https://github.com/zeranemesis/Marioparty4/releases/download/partyboard-android-latest/android-update.json";
// A manifest can only send the app to this repository's own releases.
inline constexpr std::string_view kDownloadPrefix = "https://github.com/zeranemesis/Marioparty4/releases/download/";

struct Manifest {
    // Android's versionCode, the CI run number: the only thing compared.
    std::int64_t versionCode = 0;
    std::string version;
    // The Android ABI the APK is built for, e.g. "arm64-v8a".
    std::string abi;
    std::string commit;
    std::string downloadUrl;
    // 64 lowercase hex digits.
    std::string sha256;
    // One change per line.
    std::string notes;
};

// Nothing for a manifest with a missing or malformed field, or a download outside kDownloadPrefix.
std::optional<Manifest> parse_manifest(std::string_view json);

// Whether the manifest names a newer build of the app built for `abi`.
bool offers_update(const Manifest &manifest, std::int64_t installedVersionCode, std::string_view abi);

// --- Runtime. Only Android installs updates; elsewhere supported() is false and nothing runs. ---

enum class State : std::uint8_t {
    Idle,
    Checking,
    UpToDate,
    Available,
    // The download was handed to the browser; the player opens the file to install it.
    Downloading,
    Failed,
};

struct Status {
    State state = State::Idle;
    // Valid from Available on.
    Manifest manifest;
    std::int64_t installedVersionCode = 0;
    // When Failed: a fixed English sentence, translated by the UI, and what went wrong, which is not.
    std::string error;
    std::string detail;
    // The last check was the automatic one: the UI keeps quiet unless it found an update.
    bool quiet = false;
};

bool supported() noexcept;
// Fetches the manifest on a worker thread, unless a check or an install is already running. A quiet
// check that fails (no network, most often) goes back to Idle: the game stays playable offline.
void check(bool quiet);
// Opens the download of the build the last check found in the browser. Call it from the UI thread.
void download();
Status status();

} // namespace partyboard::update
