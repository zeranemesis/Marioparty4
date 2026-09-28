// Checks the update manifest parser (src/port/app_update_manifest.cpp) against manifests shaped
// like the ones platforms/android/scripts/publish-update.sh writes.

#include "port/app_update.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace partyboard::update;

namespace {

int failures = 0;

void expect(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const std::string kHash = "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789ABCDEF";
const std::string kUrl =
    "https://github.com/zeranemesis/Marioparty4/releases/download/partyboard-android-latest/PartyBoard-android-arm64-v8a-128.apk";

std::string manifest_with(const std::string &url, const std::string &hash, const std::string &code = "128")
{
    return R"j({"versionCode": )j" + code + R"j(, "version": "0.1.0 (build 128)", "abi": "arm64-v8a",
        "commit": "d8bea614d2079d91582f755fcc87fb831d9eb535", "downloadUrl": ")j" + url + R"j(",
        "sha256": ")j" + hash + R"j(", "notes": "- Android: updates from GitHub\n- PAL: accents"})j";
}

} // namespace

int main()
{
    const auto parsed = parse_manifest(manifest_with(kUrl, kHash));
    expect(parsed.has_value(), "a published manifest parses");
    if (parsed) {
        expect(parsed->versionCode == 128, "versionCode");
        expect(parsed->version == "0.1.0 (build 128)", "version");
        expect(parsed->abi == "arm64-v8a", "abi");
        expect(parsed->downloadUrl == kUrl, "downloadUrl");
        expect(parsed->sha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "sha256 is lowercased");
        expect(parsed->notes == "- Android: updates from GitHub\n- PAL: accents", "notes keep their lines");

        expect(offers_update(*parsed, 127, "arm64-v8a"), "a newer build is offered");
        expect(!offers_update(*parsed, 128, "arm64-v8a"), "the installed build is not offered again");
        expect(!offers_update(*parsed, 200, "arm64-v8a"), "an older build is never offered");
        expect(!offers_update(*parsed, 1, "x86_64"), "another ABI's APK is not offered");
        expect(!offers_update(*parsed, 1, ""), "an unknown ABI gets nothing");
    }

    expect(!parse_manifest("").has_value(), "empty text");
    expect(!parse_manifest("[1, 2]").has_value(), "not an object");
    expect(!parse_manifest("{\"versionCode\": 3").has_value(), "truncated JSON");
    expect(!parse_manifest(manifest_with(kUrl, kHash, "0")).has_value(), "versionCode 0");
    expect(!parse_manifest(manifest_with(kUrl, kHash, "\"128\"")).has_value(), "versionCode as a string");
    expect(!parse_manifest(manifest_with(kUrl, kHash.substr(1))).has_value(), "short hash");
    expect(!parse_manifest(manifest_with(kUrl, "g" + kHash.substr(1))).has_value(), "hash with a non-hex digit");
    expect(!parse_manifest(manifest_with("https://example.com/PartyBoard.apk", kHash)).has_value(), "download off GitHub");
    expect(!parse_manifest(manifest_with("http://github.com/zeranemesis/Marioparty4/releases/download/x/a.apk", kHash))
                .has_value(),
        "plain HTTP download");
    expect(!parse_manifest(manifest_with("https://github.com/someone/else/releases/download/x/a.apk", kHash)).has_value(),
        "another repository");
    expect(!parse_manifest(manifest_with("https://github.com/zeranemesis/Marioparty4/releases/download/../../x", kHash))
                .has_value(),
        "parent directory");
    expect(!parse_manifest(manifest_with("https://github.com/zeranemesis/Marioparty4/releases/download/x/a.apk?y=1", kHash))
                .has_value(),
        "query string");

    const auto minimal = parse_manifest(R"({"versionCode": 5, "version": "5", "abi": "x86_64", "downloadUrl": ")" + kUrl
        + R"(", "sha256": ")" + kHash + R"("})");
    expect(minimal.has_value() && minimal->notes.empty() && minimal->commit.empty(), "commit and notes are optional");

    if (failures == 0) {
        std::puts("app_update_test: all checks passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
