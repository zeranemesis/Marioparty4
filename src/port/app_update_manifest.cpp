// The update manifest (android-update.json): parsing and the newer-build test. No platform code,
// so tools/tests/app_update_test.cpp builds it on its own.

#include "port/app_update.hpp"

#include <nlohmann/json.hpp>

namespace partyboard::update {
namespace {

    using json = nlohmann::json;

    // Bounds a manifest cannot need; they keep a broken one from filling the menu.
    constexpr std::size_t kMaxFieldLength = 512;
    constexpr std::size_t kMaxNotesLength = 4000;

    std::optional<std::string> text_field(const json &document, const char *key, std::size_t maxLength)
    {
        const auto found = document.find(key);
        if (found == document.end() || !found->is_string()) {
            return std::nullopt;
        }
        auto value = found->get<std::string>();
        if (value.size() > maxLength) {
            return std::nullopt;
        }
        return value;
    }

    bool is_sha256(std::string &hash)
    {
        if (hash.size() != 64) {
            return false;
        }
        for (char &c : hash) {
            if (c >= 'A' && c <= 'F') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    // A file name right under a release tag: no query, fragment, parent directory or control character.
    bool is_release_download(std::string_view url)
    {
        if (!url.starts_with(kDownloadPrefix) || url.size() == kDownloadPrefix.size()) {
            return false;
        }
        const auto rest = url.substr(kDownloadPrefix.size());
        if (rest.find("..") != std::string_view::npos) {
            return false;
        }
        for (const unsigned char c : rest) {
            if (c <= 0x20 || c == 0x7f || c == '?' || c == '#' || c == '\\') {
                return false;
            }
        }
        return true;
    }

} // namespace

std::optional<Manifest> parse_manifest(std::string_view text)
{
    const json document = json::parse(text.begin(), text.end(), nullptr, false);
    if (!document.is_object()) {
        return std::nullopt;
    }

    Manifest manifest;
    const auto code = document.find("versionCode");
    if (code == document.end() || !code->is_number_integer()) {
        return std::nullopt;
    }
    manifest.versionCode = code->get<std::int64_t>();
    if (manifest.versionCode <= 0) {
        return std::nullopt;
    }

    auto version = text_field(document, "version", kMaxFieldLength);
    auto abi = text_field(document, "abi", kMaxFieldLength);
    auto url = text_field(document, "downloadUrl", kMaxFieldLength);
    auto hash = text_field(document, "sha256", kMaxFieldLength);
    if (!version || version->empty() || !abi || abi->empty() || !url || !is_release_download(*url) || !hash
        || !is_sha256(*hash))
    {
        return std::nullopt;
    }
    manifest.version = std::move(*version);
    manifest.abi = std::move(*abi);
    manifest.downloadUrl = std::move(*url);
    manifest.sha256 = std::move(*hash);
    // Informative only: a manifest without them still updates.
    manifest.commit = text_field(document, "commit", kMaxFieldLength).value_or("");
    if (auto notes = text_field(document, "notes", kMaxNotesLength)) {
        manifest.notes = std::move(*notes);
    }
    return manifest;
}

bool offers_update(const Manifest &manifest, std::int64_t installedVersionCode, std::string_view abi)
{
    return !abi.empty() && manifest.abi == abi && manifest.versionCode > installedVersionCode;
}

} // namespace partyboard::update
