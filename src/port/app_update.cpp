// Android app updates from GitHub (port/app_update.hpp). The manifest is fetched with
// port/http.hpp; the download, the SHA-256 check and the hand-off to Android's installer run in
// Java (PartyBoardUpdater), reached through PartyBoardActivity.

#include "port/app_update.hpp"

#include "port/http.hpp"

#include <aurora/lib/logging.hpp>
#include <fmt/format.h>

#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_timer.h>
#include <jni.h>
#endif

namespace partyboard::update {
namespace {

    std::mutex gMutex;
    Status gStatus;

#ifdef __ANDROID__
    aurora::Module UpdateLog("partyboard::update");

    // A check or an install owns a worker thread.
    bool gWorking = false;

#if defined(__aarch64__)
    constexpr std::string_view kAbi = "arm64-v8a";
#elif defined(__x86_64__)
    constexpr std::string_view kAbi = "x86_64";
#else
    constexpr std::string_view kAbi = "";
#endif

    constexpr Uint64 kPollIntervalMs = 250;

    // One call into PartyBoardActivity. The class comes from the activity object rather than
    // FindClass, which cannot see the app's classes from a thread the VM did not start.
    class ActivityCall {
    public:
        ActivityCall()
        {
            mEnv = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
            if (mEnv == nullptr) {
                return;
            }
            if (mEnv->PushLocalFrame(16) != 0) {
                mEnv->ExceptionClear();
                mEnv = nullptr;
                return;
            }
            mActivity = static_cast<jobject>(SDL_GetAndroidActivity());
            if (mActivity != nullptr) {
                mClass = mEnv->GetObjectClass(mActivity);
            }
            clear_exception();
        }

        ~ActivityCall()
        {
            if (mEnv != nullptr) {
                clear_exception();
                mEnv->PopLocalFrame(nullptr);
            }
        }

        ActivityCall(const ActivityCall &) = delete;
        ActivityCall &operator=(const ActivityCall &) = delete;

        jmethodID method(const char *name, const char *signature)
        {
            if (mEnv == nullptr || mActivity == nullptr || mClass == nullptr) {
                return nullptr;
            }
            const jmethodID id = mEnv->GetMethodID(mClass, name, signature);
            return clear_exception() ? nullptr : id;
        }

        std::int64_t call_long(const char *name)
        {
            const jmethodID id = method(name, "()J");
            if (id == nullptr) {
                return 0;
            }
            const jlong value = mEnv->CallLongMethod(mActivity, id);
            return clear_exception() ? 0 : static_cast<std::int64_t>(value);
        }

        int call_int(const char *name, int fallback)
        {
            const jmethodID id = method(name, "()I");
            if (id == nullptr) {
                return fallback;
            }
            const jint value = mEnv->CallIntMethod(mActivity, id);
            return clear_exception() ? fallback : static_cast<int>(value);
        }

        // Nothing when Java returned null.
        std::optional<std::string> call_string(const char *name, const char *signature, std::initializer_list<jvalue> args,
            bool &ok)
        {
            ok = false;
            const jmethodID id = method(name, signature);
            if (id == nullptr) {
                return std::nullopt;
            }
            std::vector<jvalue> values(args);
            auto *result = static_cast<jstring>(mEnv->CallObjectMethodA(mActivity, id, values.data()));
            if (clear_exception()) {
                return std::nullopt;
            }
            ok = true;
            if (result == nullptr) {
                return std::nullopt;
            }
            const char *utf = mEnv->GetStringUTFChars(result, nullptr);
            if (utf == nullptr) {
                clear_exception();
                return std::string {};
            }
            std::string text(utf);
            mEnv->ReleaseStringUTFChars(result, utf);
            return text;
        }

        jstring string(const std::string &text) { return mEnv != nullptr ? mEnv->NewStringUTF(text.c_str()) : nullptr; }

    private:
        bool clear_exception()
        {
            if (mEnv == nullptr || !mEnv->ExceptionCheck()) {
                return false;
            }
            mEnv->ExceptionDescribe();
            mEnv->ExceptionClear();
            return true;
        }

        JNIEnv *mEnv = nullptr;
        jobject mActivity = nullptr;
        jclass mClass = nullptr;
    };

    void fail(std::string error, std::string detail = {})
    {
        UpdateLog.warn("{} {}", error, detail);
        std::lock_guard lock(gMutex);
        // An automatic check says nothing when it fails: most often there is no network.
        const bool silent = gStatus.quiet && gStatus.state == State::Checking;
        gStatus.state = silent ? State::Idle : State::Failed;
        gStatus.error = std::move(error);
        gStatus.detail = std::move(detail);
        gStatus.progress = -1;
        gWorking = false;
    }

    // Java reports "cancelled" when the player declines Android's prompt; anything else is the
    // installer's own message.
    void fail_install(const std::string &failure)
    {
        if (failure == "cancelled") {
            fail("The update was cancelled.");
        }
        // Signed with another key: the installed build came from a run that signed with a key of
        // its own. Only uninstalling (which deletes the saves) changes the key.
        else if (failure.find("INCOMPATIBLE") != std::string::npos || failure.find("signature") != std::string::npos) {
            fail("This build is signed with a different key than the installed one. Back up your saves, uninstall "
                 "Party Board once, then install the update from GitHub.",
                failure);
        }
        else {
            fail("The update could not be installed.", failure);
        }
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
        std::int64_t installed = 0;
        {
            ActivityCall call;
            installed = call.call_long("getInstalledVersionCode");
        }
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

    void run_install(Manifest manifest)
    {
        bool ok = false;
        std::optional<std::string> error;
        {
            ActivityCall call;
            error = call.call_string("installUpdate", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                { jvalue { .l = call.string(manifest.downloadUrl) }, jvalue { .l = call.string(manifest.sha256) } }, ok);
        }
        if (!ok) {
            fail("The update could not be started.");
            return;
        }
        if (error) {
            if (*error == "checksum") {
                fail("The download does not match the SHA-256 GitHub announced. Nothing was installed.");
            }
            else {
                fail("The update could not be downloaded.", *error);
            }
            return;
        }
        UpdateLog.info("Build {} handed to the Android installer", manifest.versionCode);
        std::lock_guard lock(gMutex);
        gStatus.state = State::Installing;
        gStatus.progress = 100;
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
        if (gWorking || gStatus.state == State::Installing) {
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

void install()
{
#ifdef __ANDROID__
    Manifest manifest;
    {
        std::lock_guard lock(gMutex);
        if (gWorking || gStatus.manifest.versionCode <= gStatus.installedVersionCode) {
            return;
        }
        gWorking = true;
        gStatus.state = State::Downloading;
        gStatus.progress = 0;
        gStatus.error.clear();
        gStatus.detail.clear();
        manifest = gStatus.manifest;
    }
    std::thread(run_install, std::move(manifest)).detach();
#endif
}

Status status()
{
#ifdef __ANDROID__
    State state;
    {
        std::lock_guard lock(gMutex);
        state = gStatus.state;
    }
    // Both answers come from Java: poll them, a few times a second, outside the lock.
    static Uint64 lastPoll = 0;
    const Uint64 now = SDL_GetTicks();
    if ((state == State::Downloading || state == State::Installing) && now - lastPoll >= kPollIntervalMs) {
        lastPoll = now;
        ActivityCall call;
        if (state == State::Downloading) {
            const int progress = call.call_int("getUpdateProgress", -1);
            std::lock_guard lock(gMutex);
            if (gStatus.state == State::Downloading) {
                gStatus.progress = progress;
            }
        }
        else {
            bool ok = false;
            if (auto failure = call.call_string("takeUpdateInstallFailure", "()Ljava/lang/String;", {}, ok)) {
                fail_install(*failure);
            }
        }
    }
#endif
    std::lock_guard lock(gMutex);
    return gStatus;
}

} // namespace partyboard::update
