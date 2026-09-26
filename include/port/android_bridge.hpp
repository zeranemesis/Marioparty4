#pragma once

#include <string>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>

namespace partyboard::android {

// Calls a method of PartyBoardActivity from the SDL thread: `call(env, activity, method)` runs only
// when the method exists. Returns false with no activity, no such method, or a Java exception.
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
    const bool failed = env->ExceptionCheck();
    if (failed) {
        env->ExceptionClear();
    }
    if (type != nullptr) {
        env->DeleteLocalRef(type);
    }
    env->DeleteLocalRef(activity);
    return method != nullptr && !failed;
}

inline std::string to_string(JNIEnv *env, jstring value)
{
    if (value == nullptr) {
        return {};
    }
    const char *utf8 = env->GetStringUTFChars(value, nullptr);
    std::string out = utf8 != nullptr ? utf8 : "";
    if (utf8 != nullptr) {
        env->ReleaseStringUTFChars(value, utf8);
    }
    return out;
}

} // namespace partyboard::android
#endif
