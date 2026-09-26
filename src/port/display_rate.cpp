#include "port/display_rate.hpp"

#include <algorithm>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace partyboard::display {

#ifdef __ANDROID__
namespace {
    // Calls a method of PartyBoardActivity; false when there is no activity or no such method.
    template <typename Call>
    bool with_activity(const char *name, const char *signature, Call &&call)
    {
        auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
        auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if (env == nullptr || activity == nullptr) {
            return false;
        }
        jclass type = env->GetObjectClass(activity);
        jmethodID method = type != nullptr ? env->GetMethodID(type, name, signature) : nullptr;
        if (method != nullptr) {
            call(env, activity, method);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            method = nullptr;
        }
        if (type != nullptr) {
            env->DeleteLocalRef(type);
        }
        env->DeleteLocalRef(activity);
        return method != nullptr;
    }
} // namespace
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
