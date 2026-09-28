// Android app updates from GitHub (port/app_update.hpp). The manifest is fetched with
// port/http.hpp; the APK itself is downloaded by the browser, the way the first install was.

#include "port/app_update.hpp"

#include "port/http.hpp"

#include <aurora/lib/logging.hpp>
#include <fmt/format.h>

#include <mutex>
#include <thread>

#ifdef __ANDROID__
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace partyboard::update {
namespace {

    std::mutex gMutex;
    Status gStatus;

#ifdef __ANDROID__
    aurora::Module UpdateLog("partyboard::update");

    // A check owns a worker thread.
    bool gWorking = false;

#if defined(__aarch64__)
    constexpr std::string_view kAbi = "arm64-v8a";
#elif defined(__x86_64__)
    constexpr std::string_view kAbi = "x86_64";
#else
    constexpr std::string_view kAbi = "";
#endif

    // PartyBoardActivity.getInstalledVersionCode(). The class comes from the activity object
    // rather than FindClass, which cannot see the app's classes from a thread the VM did not start.
    std::int64_t installed_version_code()
    {
        auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
        if (env == nullptr || env->PushLocalFrame(8) != 0) {
            if (env != nullptr) {
                env->ExceptionClear();
            }
            return 0;
        }
        std::int64_t code = 0;
        if (auto *activity = static_cast<jobject>(SDL_GetAndroidActivity())) {
            jclass activityClass = env->GetObjectClass(activity);
            const jmethodID method =
                activityClass != nullptr ? env->GetMethodID(activityClass, "getInstalledVersionCode", "()J") : nullptr;
            if (method != nullptr && !env->ExceptionCheck()) {
                code = static_cast<std::int64_t>(env->CallLongMethod(activity, method));
            }
        }
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            code = 0;
        }
        env->PopLocalFrame(nullptr);
        return code;
    }

    void fail(std::string error, std::string detail = {})
    {
        UpdateLog.warn("{} {}", error, detail);
        std::lock_guard lock(gMutex);
        // An automatic check says nothing when it fails: most often there is no network.
        const bool silent = gStatus.quiet && gStatus.state == State::Checking;
        gStatus.state = silent ? State::Idle : State::Failed;
        gStatus.error = std::move(error);
        gStatus.detail = std::move(detail);
        gWorking = false;
    }

    void run_check()
    {
        const auto response = http::get(std::string(kManifestUrl), "PartyBoard-Updater/1");
        if (response.status != 200) {
            if (response.status == 0) {
                fail("Could not reach GitHub.", response.error);
            }
            else {
                fail("GitHub did not send the update manifest.", fmt::format("HTTP {}", response.status));
            }
            return;
        }
        auto manifest = parse_manifest(response.body);
        if (!manifest) {
            fail("The update manifest on GitHub is invalid.");
            return;
        }
        const std::int64_t installed = installed_version_code();
        const bool newer = offers_update(*manifest, installed, kAbi);
        UpdateLog.info("Installed build {}, GitHub has build {} for {}{}", installed, manifest->versionCode,
            manifest->abi, newer ? ": update available" : "");

        std::lock_guard lock(gMutex);
        gStatus.state = newer ? State::Available : State::UpToDate;
        gStatus.manifest = std::move(*manifest);
        gStatus.installedVersionCode = installed;
        gStatus.error.clear();
        gStatus.detail.clear();
        gWorking = false;
    }
#endif

} // namespace

bool supported() noexcept
{
#ifdef __ANDROID__
    return !std::string_view(kAbi).empty();
#else
    return false;
#endif
}

void check([[maybe_unused]] bool quiet)
{
#ifdef __ANDROID__
    if (!supported()) {
        return;
    }
    {
        std::lock_guard lock(gMutex);
        if (gWorking) {
            return;
        }
        gWorking = true;
        gStatus.state = State::Checking;
        gStatus.quiet = quiet;
        gStatus.error.clear();
        gStatus.detail.clear();
    }
    std::thread(run_check).detach();
#endif
}

void download()
{
#ifdef __ANDROID__
    std::string url;
    {
        std::lock_guard lock(gMutex);
        if (gWorking || gStatus.manifest.versionCode <= gStatus.installedVersionCode) {
            return;
        }
        url = gStatus.manifest.downloadUrl;
    }
    // The browser downloads the APK and Android installs it over this one, keeping the saves:
    // no install permission in the app, nothing for Play Protect to hold against it.
    if (!SDL_OpenURL(url.c_str())) {
        fail("Could not open the download.", SDL_GetError());
        return;
    }
    UpdateLog.info("Opened {} in the browser", url);
    std::lock_guard lock(gMutex);
    gStatus.state = State::Downloading;
    gStatus.error.clear();
    gStatus.detail.clear();
#endif
}

Status status()
{
    std::lock_guard lock(gMutex);
    return gStatus;
}

} // namespace partyboard::update
