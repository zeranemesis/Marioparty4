#include "port/display_rate.hpp"

#include <algorithm>

#include "port/android_bridge.hpp"

namespace partyboard::display {

#ifdef __ANDROID__
using partyboard::android::with_activity;
#endif

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
