#include "port/config.hpp"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "absl/container/flat_hash_map.h"

#include "aurora/lib/logging.hpp"
#include "port/io.hpp"
#include "port/settings.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "port/main.h"

using namespace partyboard::config;

constexpr auto ConfigFileName = "config.json";

using json = nlohmann::json;

aurora::Module PartyBoardConfigLog("partyboard::config");

static absl::flat_hash_map<std::string_view, ConfigVarBase*> RegisteredConfigVars;
static bool RegistrationDone = false;

static std::u8string GetConfigJsonPath() {
    return (PartyBoard_ConfigPath / ConfigFileName).u8string();
}

ConfigVarBase::ConfigVarBase(const char* name, const ConfigImplBase* impl) : name(name), registered(false), layer(ConfigVarLayer::Default), impl(impl) {
}

const char* ConfigVarBase::getName() const noexcept {
    return name;
}

const ConfigImplBase* ConfigVarBase::getImpl() const noexcept {
    return impl;
}

template <typename T>
static T sanitizeEnumValue(const ConfigVar<T>& cVar, T value) {
    if constexpr (std::is_enum_v<T>) {
        using Underlying = std::underlying_type_t<T>;
        const Underlying raw = static_cast<Underlying>(value);
        const Underlying min = static_cast<Underlying>(ConfigEnumRange<T>::min);
        const Underlying max = static_cast<Underlying>(ConfigEnumRange<T>::max);
        if (raw < min || raw > max) {
            return cVar.getDefaultValue();
        }
    }

    return value;
}

template<ConfigValue T>
void ConfigImpl<T>::loadFromJson(ConfigVar<T>& cVar, const json& jsonValue) {
    cVar.setValue(sanitizeEnumValue(cVar, jsonValue.get<T>()), false);
}

template<ConfigValue T>
nlohmann::json ConfigImpl<T>::dumpToJson(const ConfigVar<T>& cVar) {
    return cVar.getValue();
}

template<ConfigValue T> requires std::is_integral_v<T> && std::is_signed_v<T>
static void loadFromArgImpl(ConfigVar<T>& cVar, const std::string_view stringValue) {
    const std::string str(stringValue);
    const auto result = std::stoll(str);
    if (result >= std::numeric_limits<T>::min() && result <= std::numeric_limits<T>::max()) {
        cVar.setOverrideValue(result);
    } else {
        throw std::out_of_range("Value is too large");
    }
}

template<ConfigValue T> requires std::is_integral_v<T> && std::is_unsigned_v<T>
static void loadFromArgImpl(ConfigVar<T>& cVar, const std::string_view stringValue) {
    const std::string str(stringValue);
    const auto result = std::stoull(str);
    if (result <= std::numeric_limits<T>::max()) {
        cVar.setOverrideValue(result);
    } else {
        throw std::out_of_range("Value is too large");
    }
}

static void loadFromArgImpl(ConfigVar<f32>& cVar, const std::string_view stringValue) {
    const std::string str(stringValue);
    const auto result = std::stof(str);
    cVar.setOverrideValue(result);
}

static void loadFromArgImpl(ConfigVar<f64>& cVar, const std::string_view stringValue) {
    const std::string str(stringValue);
    const auto result = std::stod(str);
    cVar.setOverrideValue(result);
}

static void loadFromArgImpl(ConfigVar<std::string>& cVar, const std::string_view stringValue) {
    cVar.setOverrideValue(std::string(stringValue));
}

template<ConfigValue T> requires std::is_enum_v<T>
static void loadFromArgImpl(ConfigVar<T>& cVar, const std::string_view stringValue) {
    using Underlying = std::underlying_type_t<T>;
    const std::string str(stringValue);

    if constexpr (std::is_signed_v<Underlying>) {
        const auto result = std::stoll(str);
        if (result >= std::numeric_limits<Underlying>::min() && result <= std::numeric_limits<Underlying>::max()) {
            cVar.setOverrideValue(sanitizeEnumValue(cVar, static_cast<T>(result)));
        } else {
            throw std::out_of_range("Value is too large");
        }
    } else {
        const auto result = std::stoull(str);
        if (result <= std::numeric_limits<Underlying>::max()) {
            cVar.setOverrideValue(sanitizeEnumValue(cVar, static_cast<T>(result)));
        } else {
            throw std::out_of_range("Value is too large");
        }
    }
}

template<ConfigValue T>
void ConfigImpl<T>::loadFromArg(ConfigVar<T>& cVar, const std::string_view stringValue) {
    loadFromArgImpl(cVar, stringValue);
}

template<>
void ConfigImpl<bool>::loadFromArg(ConfigVar<bool>& cVar, const std::string_view stringValue) {
    if (stringValue == "1" || stringValue == "TRUE" || stringValue == "true" || stringValue == "True") {
        cVar.setOverrideValue(true);
    } else if (stringValue == "0" || stringValue == "FALSE" || stringValue == "false" || stringValue == "False") {
        cVar.setOverrideValue(false);
    } else {
        throw InvalidConfigError("Value cannot be parsed as boolean");
    }
}

// My IDE is convinced this namespace is necessary. It shouldn't be AFAICT?
namespace partyboard::config {
    template class ConfigImpl<bool>;
    template class ConfigImpl<s8>;
    template class ConfigImpl<u8>;
    template class ConfigImpl<s16>;
    template class ConfigImpl<u16>;
    template class ConfigImpl<s32>;
    template class ConfigImpl<u32>;
    template class ConfigImpl<s64>;
    template class ConfigImpl<u64>;
    template class ConfigImpl<f32>;
    template class ConfigImpl<f64>;
    template class ConfigImpl<std::string>;
    template class ConfigImpl<partyboard::DiscVerificationState>;
    template class ConfigImpl<partyboard::GameLanguage>;
}

void partyboard::config::Register(ConfigVarBase& configVar) {
    const auto& name = configVar.getName();
    if (RegistrationDone) {
        PartyBoardConfigLog.fatal("Tried to register CVar {} after registrations closed!", name);
    }

    if (RegisteredConfigVars.contains(name)) {
        PartyBoardConfigLog.fatal("Tried to register CVar {} twice!", name);
    }

    RegisteredConfigVars[name] = &configVar;
    configVar.markRegistered();
}

void ConfigVarBase::markRegistered() {
    if (registered)
        abort();

    registered = true;
}

void partyboard::config::FinishRegistration() {
    RegistrationDone = true;
}

void partyboard::config::LoadFromUserPreferences() {
    const auto configJsonPath = GetConfigJsonPath();
    if (configJsonPath.empty()) {
        return;
    }
    LoadFromFileName(reinterpret_cast<const char*>(configJsonPath.c_str()));
}

static void LoadFromPath(const char* path) {
    auto data = partyboard::io::FileStream::ReadAllBytes(path);

    json j = json::parse(data);
    if (!j.is_object()) {
        PartyBoardConfigLog.error("Config JSON is not an object!");
        return;
    }

    for (const auto& el : j.items()) {
        const auto& key = el.key();
        auto configVar = RegisteredConfigVars.find(key);
        if (configVar == RegisteredConfigVars.end()) {
            PartyBoardConfigLog.error("Unknown key '{}' found in config!", key);
            continue;
        }

        try {
            configVar->second->getImpl()->loadFromJson(*configVar->second, el.value());
        } catch (std::exception& e) {
            PartyBoardConfigLog.error("Failed to load key '{}' from config: {}", key, e.what());
        }
    }
}

void partyboard::config::LoadFromFileName(const char* path) {
    if (!RegistrationDone) {
        PartyBoardConfigLog.fatal("Registration not finished yet!");
    }

    PartyBoardConfigLog.info("Loading config from '{}'", path);

    try {
        LoadFromPath(path);
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::no_such_file_or_directory) {
            PartyBoardConfigLog.info("Config file did not exist, staying with defaults");
        } else {
            PartyBoardConfigLog.error("Failed to load from config! {}", e.what());
        }
    } catch (const std::exception& e) {
        // A file cut short by a crash or a full disk is not valid JSON. Starting
        // with defaults is recoverable; refusing to start is not, and on a phone
        // the player has no way to repair the file by hand.
        PartyBoardConfigLog.error("Config file is unreadable, staying with defaults: {}", e.what());
    }
}

void partyboard::config::Save() {
    const auto configJsonPath = GetConfigJsonPath();
    if (configJsonPath.empty()) {
        return;
    }

    PartyBoardConfigLog.info(
        "Saving config to '{}'",
        reinterpret_cast<const char*>(configJsonPath.c_str()));

    json j;

    for (const auto& pair : RegisteredConfigVars) {
        if (pair.second->getLayer() == ConfigVarLayer::Value) {
            j[pair.first] = pair.second->getImpl()->dumpToJson(*pair.second);
        }
    }

    // Write a temporary file and rename it over the config, so that being
    // killed half-way (Android does that to background apps) leaves either the
    // old file or the new one, never a truncated one.
    const std::filesystem::path path(configJsonPath);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    const std::string text = j.dump(4);
    try {
        {
            auto file = io::FileStream::Create(temporary);
            file.Write(text.data(), text.size());
            auto* handle = static_cast<FILE*>(file.GetFileHandle());
            if (std::fflush(handle) != 0) {
                throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)));
            }
#ifndef _WIN32
            fsync(fileno(handle));
#endif
        }

        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            // Should a filesystem refuse to rename over an existing file, fall
            // back to writing in place, which is what this always did.
            PartyBoardConfigLog.warn("Could not replace config atomically ({}), writing it in place",
                error.message());
            std::filesystem::remove(temporary, error);
            io::FileStream::WriteAllText(path, text);
        }
    } catch (const std::exception& e) {
        // Called from menus and from lifecycle callbacks, none of which can
        // handle an exception: a full disk must not take the game down.
        PartyBoardConfigLog.error("Failed to save config to '{}': {}",
            reinterpret_cast<const char*>(configJsonPath.c_str()), e.what());
    }
}

ConfigVarBase* partyboard::config::GetConfigVar(std::string_view name) {
    const auto configVar = RegisteredConfigVars.find(name);
    if (configVar != RegisteredConfigVars.end()) {
        return configVar->second;
    }

    return nullptr;
}
