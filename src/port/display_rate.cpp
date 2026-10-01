#include "port/display_rate.hpp"

#include <algorithm>
#ifdef __ANDROID__
#include <dlfcn.h>
#endif

#include "port/android_bridge.hpp"

namespace partyboard::display {

#ifdef __ANDROID__
using partyboard::android::with_activity;
#endif

int headset_frame_rate()
{
#ifdef __ANDROID__
    using RateFn = float (*)();
    // Retry resolution: the XR library can start after the game library.
    static RateFn rate = nullptr;
    if (!rate) {
        void *library = dlopen("libpartyboard_quest.so", RTLD_NOW | RTLD_NOLOAD);
        if (library) {
            rate = reinterpret_cast<RateFn>(dlsym(library, "PartyBoardQuest_ActiveRefreshRate"));
            dlclose(library);
        }
    }
    if (rate) return static_cast<int>(rate() + 0.5f);
#endif
    return 0;
}

int64_t headset_frame_start([[maybe_unused]] int64_t nowNs, [[maybe_unused]] int targetFps)
{
#ifdef __ANDROID__
    using StartFn = int64_t (*)(int64_t, float);
    static StartFn start = nullptr;
    if (!start) {
        void *library = dlopen("libpartyboard_quest.so", RTLD_NOW | RTLD_NOLOAD);
        if (library) {
            start = reinterpret_cast<StartFn>(dlsym(library, "PartyBoardQuest_NextFrameStart"));
            dlclose(library);
        }
    }
    if (start) return start(nowNs, static_cast<float>(targetFps));
#endif
    return 0;
}

std::vector<int> supported_refresh_rates()
{
    std::vector<int> rates;
#ifdef __ANDROID__
    with_activity("getSupportedRefreshRates", "()[F", [&](JNIEnv *env, jobject activity, jmethodID method) {
        auto array = static_cast<jfloatArray>(env->CallObjectMethod(activity, method));
        if (array == nullptr) {
            return;
        }
        const jsize count = env->GetArrayLength(array);
        std::vector<jfloat> values(static_cast<std::size_t>(count));
        env->GetFloatArrayRegion(array, 0, count, values.data());
        for (const jfloat value : values) {
            rates.push_back(static_cast<int>(value + 0.5f));
        }
        env->DeleteLocalRef(array);
    });
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
#endif
    return rates;
}

void request_frame_rate([[maybe_unused]] int fps)
{
#ifdef __ANDROID__
    with_activity("setPreferredFrameRate", "(F)V", [&](JNIEnv *env, jobject activity, jmethodID method) {
        env->CallVoidMethod(activity, method, static_cast<jfloat>(fps));
    });
#endif
}

} // namespace partyboard::display
