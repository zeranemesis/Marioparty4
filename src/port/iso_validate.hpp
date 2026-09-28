// Credits: TwilitRealm

#pragma once

#include <atomic>
#include <port/disc_version.hpp>
#include <port/settings.h>

namespace partyboard::iso {
struct KnownDisc;

enum class ValidationError : uint8_t {
    Unknown = 0,
    IOError,
    InvalidImage,
    WrongGame,
    WrongVersion,
    Canceled,
    HashMismatch,
    Success
};

struct VerificationStatus {
    std::atomic_size_t bytesRead = 0;
    std::atomic_size_t bytesTotal = 0;
    const KnownDisc* knownDisc = nullptr;
    std::atomic_bool shouldCancel = false;
};

struct DiscInfo {
    bool isPal = false;
    // Only meaningful once the disc was recognised as Mario Party 4 (`known`).
    bool known = false;
    version::DiscRegion region = version::DiscRegion::Usa;
    uint8_t revision = 0;
};

ValidationError inspect(const char* path, DiscInfo& info);
ValidationError validate(const char* path, VerificationStatus& status, DiscInfo& info);
bool isPal(const char* path);
void log_verification_state(std::string_view path, DiscVerificationState state);

}  // namespace partyboard::iso
