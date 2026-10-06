// The game's reading of CubeShelf's profile export and sealed presence, checked against files
// the C# code wrote (tools/tests/fixtures, produced by CubeShelf.Core: ProfileTransfer.Export and
// SealedPresence.Seal). Standalone: needs mbedcrypto and nlohmann/json.
//   c++ -std=c++20 -Iinclude -I<mbedtls>/include -I<json>/single_include
//     tools/tests/cubeshelf_social_test.cpp src/port/online/cubeshelf_social.cpp -L<mbedtls> -lmbedcrypto
//   ./a.out tools/tests/fixtures
#include "port/online/cubeshelf_social.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace partyboard::online::cubeshelf;

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string read(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

constexpr std::int64_t kPublished = 1790337600; // 2026-09-25T12:00:00Z

} // namespace

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : "tools/tests/fixtures";
    const std::string exported = read(dir + "/cubeshelf_profile.txt");
    check(!exported.empty(), "profile fixture present");

    ImportError error = ImportError::None;
    check(!import_profile(exported, "correct horse batterY", error) && error == ImportError::WrongPassphrase, "wrong passphrase refused");
    check(!import_profile("CSF2-abc", "x", error) && error == ImportError::NotAProfile, "a friend code is not a profile");
    check(!import_profile(exported.substr(0, exported.size() - 12), "correct horse battery", error), "a truncated file is refused");

    // Line breaks picked up on the way (mail, chat) do not matter.
    std::string wrapped;
    for (std::size_t i = 0; i < exported.size(); i += 64) {
        wrapped += exported.substr(i, 64) + "\r\n";
    }
    auto profile = import_profile(wrapped, "correct horse battery", error);
    check(profile.has_value() && error == ImportError::None, "the C# export imports");
    if (!profile) {
        return EXIT_FAILURE;
    }
    check(profile->name == "Zera", "pseudo");
    check(handle(profile->name, profile->publicKey) == "Zera#3093", "own handle matches PeerName.Handle");
    check(profile->exportedAtUnix == kPublished, "export time");
    check(profile->friends.size() == 2, "both friends arrive");
    const auto &alex = profile->friends[0];
    check(alex.name == "Alex" && alex.lastSequence == 5 && !alex.paused && alex.presenceUrl == "https://example.org/alex.json", "Alex as stored");
    check(profile->friends[1].name == "Sam" && profile->friends[1].paused, "Sam arrives paused");

    // Stored and read back, it is the same profile.
    const auto stored = deserialize_profile(serialize_profile(*profile));
    check(stored && stored->privateKey == profile->privateKey && stored->publicKey == profile->publicKey && stored->friends.size() == 2
            && stored->friends[0].lastSequence == 5,
        "the stored profile round-trips");

    PublicKey alexKey {}, samKey {};
    check(decode_public_key(alex.publicKey, alexKey) && decode_public_key(profile->friends[1].publicKey, samKey), "friend keys decode");
    check(handle(alex.name, alexKey) == "Alex#9030", "friend handle matches PeerName.Handle");
    check(handle(alex.name, alexKey, true).rfind("Alex#9030", 0) == 0 && handle(alex.name, alexKey, true).size() == 11, "long tag extends the short one");

    // Alex's document, sealed by the C# code for us and one other friend.
    const std::string document = read(dir + "/cubeshelf_presence_alex.json");
    const auto snapshot = open_presence(*profile, alexKey, document);
    check(snapshot.has_value(), "a document addressed to us opens");
    if (snapshot) {
        check(snapshot->displayName == "Alex" && snapshot->sequence == 42 && snapshot->status == Status::InGame, "snapshot fields");
        check(snapshot->currentGameTitle == "Mario Party 4" && snapshot->publishedUnix == kPublished, "current game and time");
        check(snapshot->invite.has_value() && snapshot->invite->joinPayload.rfind("PB4.", 0) == 0 && snapshot->invite->gameId == "GMPE01_00",
            "the invitation carries the lobby");
        check(invites(*snapshot, *profile, "GMPE01_00", kPublished) && invites(*snapshot, *profile, "gmpe01_00", kPublished), "it invites us");
        check(!invites(*snapshot, *profile, "GRSEAF", kPublished), "not to another game");
        check(!invites(*snapshot, *profile, "GMPE01_00", 4102444800), "not once expired");
        check(effective_status(*snapshot, kPublished + 10 * 60) == Status::InGame, "fresh presence is believed");
        check(effective_status(*snapshot, kPublished + 20 * 60) == Status::Offline, "stale presence reads offline");
        check(effective_status(*snapshot, kPublished - 10 * 60) == Status::Offline, "presence from the future is not believed");
    }
    check(!open_presence(*profile, samKey, document), "the wrong author does not open it");
    check(!open_presence(*profile, alexKey, read(dir + "/cubeshelf_presence_not_for_us.json")), "a document not addressed to us stays shut");

    std::string tampered = document;
    const auto at = tampered.find("\"p\":\"") + 10;
    tampered[at] = tampered[at] == 'A' ? 'B' : 'A';
    check(!open_presence(*profile, alexKey, tampered), "tampering is caught");
    check(!open_presence(*profile, alexKey, "{}") && !open_presence(*profile, alexKey, "not json"), "garbage is refused");
    if (snapshot) {
        check(snapshot->address.empty() && snapshot->availability.empty() && snapshot->activity.empty(),
            "a document from before 0.10 has none of the newer fields");
    }

    // Version 2: a document CubeShelf 0.10 wrote -- padded to 4 KiB with spaces after the JSON,
    // and carrying the address it lives at, an availability, the game's activity line and a note
    // sealed for one friend (which this reader leaves alone).
    const auto riley = deserialize_profile(read(dir + "/cubeshelf_device_profile_v2.json"));
    check(riley.has_value() && riley->name == "Riley" && riley->friends.size() == 1, "the v2 device profile loads");
    if (riley) {
        PublicKey kimKey {};
        check(decode_public_key(riley->friends[0].publicKey, kimKey), "Kim's key decodes");
        const auto kim = open_presence(*riley, kimKey, read(dir + "/cubeshelf_presence_kim_v2.json"));
        check(kim.has_value(), "a padded document with the newer fields opens");
        if (kim) {
            check(kim->sequence == 77 && kim->status == Status::InGame && kim->currentGameTitle == "Mario Party 4", "v2 snapshot fields");
            check(kim->address == "https://new.example.org/kim.json", "the stated address is read");
            check(kim->availability == "busy", "the availability is read");
            check(kim->activity == "Plateau de Toad \xE2\x80\x94 tour 12/20", "the game's activity line is read");
            check(effective_status(*kim, kPublished + 60) == Status::InGame, "a busy friend is still in game");
        }
    }
    check(clean_line("a\x1b[31mb\nc\xE2\x80\xAE" "d", 120) == "a[31mb cd", "controls and overrides go, a line break is a space");
    check(clean_line("  \n  ", 120).empty() && clean_line("abcdef", 3) == "abc", "blank is empty, the length is capped");

    check(sanitize_name("  Za\tra  <b># x ") == "Zara b x", "PeerName.Sanitize rules");
    check(sanitize_name(std::string(40, 'a')).size() == 32, "names are capped");
    check(parse_timestamp("2026-09-25T12:00:00+00:00") == kPublished, "offset timestamp");
    check(parse_timestamp("2026-09-25T14:00:00.1234567+02:00") == kPublished, "fraction and positive offset");
    check(parse_timestamp("2026-09-25T12:00:00Z") == kPublished, "Z timestamp");
    check(!parse_timestamp("2026-09-25 12:00") && !parse_timestamp(""), "unreadable timestamps");

    if (failures == 0) {
        std::puts("cubeshelf social tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
