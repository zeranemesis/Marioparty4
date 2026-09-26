#include "port/online/cubeshelf_social.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace partyboard::online::cubeshelf {
namespace {

    using json = nlohmann::json;
    using Bytes = std::vector<std::uint8_t>;

    // ProfileTransfer.cs
    constexpr std::string_view kProfilePrefix = "CSP1-";
    constexpr std::string_view kProfileAad = "cubeshelf-profile-v1";
    constexpr unsigned kProfileIterations = 310000;
    constexpr std::size_t kSaltLength = 16;
    constexpr std::size_t kMaximumProfileText = 1024 * 1024;

    // SealedPresence.cs / PeerIdentity.cs / PeerName.cs
    constexpr std::string_view kPairContext = "cubeshelf-friend-v1";
    constexpr std::string_view kHintContext = "cubeshelf-presence-hint-v1";
    constexpr std::string_view kPresenceAad = "cubeshelf-presence-v1";
    constexpr std::size_t kNonceLength = 12;
    constexpr std::size_t kTagLength = 16;
    constexpr std::size_t kKeyLength = 32;
    constexpr std::size_t kHintLength = 8;
    constexpr std::size_t kMaximumBoxes = 4096;
    constexpr std::size_t kMaximumPayload = 4 * 1024 * 1024;
    constexpr std::size_t kMaximumNameUnits = 32;

    // PresencePolicy.FreshnessWindow, and the five minutes of clock skew IsFresh tolerates.
    constexpr std::int64_t kFreshnessSeconds = 15 * 60;
    constexpr std::int64_t kFutureSkewSeconds = 5 * 60;

    struct Wiped {
        Bytes bytes;
        explicit Wiped(std::size_t size = 0)
            : bytes(size)
        {
        }
        ~Wiped()
        {
            if (!bytes.empty()) {
                mbedtls_platform_zeroize(bytes.data(), bytes.size());
            }
        }
        std::uint8_t *data() { return bytes.data(); }
        std::size_t size() const { return bytes.size(); }
    };

    // Only the ECDH blinding needs randomness; it is seeded once from the platform entropy.
    int random_bytes(void *, unsigned char *output, std::size_t length)
    {
        static std::mutex mutex;
        static mbedtls_entropy_context entropy;
        static mbedtls_ctr_drbg_context drbg;
        static bool seeded = false;
        std::lock_guard lock(mutex);
        if (!seeded) {
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&drbg);
            static constexpr unsigned char personal[] = "partyboard-cubeshelf";
            if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, personal, sizeof(personal) - 1) != 0) {
                return -1;
            }
            seeded = true;
        }
        return mbedtls_ctr_drbg_random(&drbg, output, length);
    }

    const std::uint8_t *bytes_of(std::string_view text)
    {
        return reinterpret_cast<const std::uint8_t *>(text.data());
    }

    void sha256(const std::uint8_t *input, std::size_t length, std::uint8_t out[32])
    {
        mbedtls_sha256(input, length, out, 0);
    }

    std::optional<Bytes> decode_base64(std::string_view text)
    {
        std::size_t length = 0;
        mbedtls_base64_decode(nullptr, 0, &length, bytes_of(text), text.size());
        Bytes out(length);
        if (mbedtls_base64_decode(out.data(), out.size(), &length, bytes_of(text), text.size()) != 0) {
            return std::nullopt;
        }
        out.resize(length);
        return out;
    }

    std::optional<Bytes> decode_base64_exact(const json &value, std::size_t expected)
    {
        if (!value.is_string()) {
            return std::nullopt;
        }
        auto decoded = decode_base64(value.get_ref<const std::string &>());
        if (!decoded || decoded->size() != expected) {
            return std::nullopt;
        }
        return decoded;
    }

    bool constant_time_equal(const std::uint8_t *a, const std::uint8_t *b, std::size_t length)
    {
        std::uint8_t diff = 0;
        for (std::size_t i = 0; i < length; ++i) {
            diff |= a[i] ^ b[i];
        }
        return diff == 0;
    }

    bool gcm_decrypt(const std::uint8_t *key, const std::uint8_t *nonce, const std::uint8_t *input, std::size_t length,
        const std::uint8_t *tag, const std::uint8_t *aad, std::size_t aadLength, std::uint8_t *output)
    {
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
            && mbedtls_gcm_auth_decrypt(&gcm, length, nonce, kNonceLength, aad, aadLength, tag, kTagLength, input, output) == 0;
        mbedtls_gcm_free(&gcm);
        return ok;
    }

    // System.Text.Json reads these case-insensitively (PropertyNameCaseInsensitive).
    const json *field(const json &object, std::string_view name)
    {
        if (!object.is_object()) {
            return nullptr;
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto &key = it.key();
            if (key.size() == name.size()
                && std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })) {
                return &it.value();
            }
        }
        return nullptr;
    }

    std::string string_field(const json &object, std::string_view name)
    {
        const auto *value = field(object, name);
        return value != nullptr && value->is_string() ? value->get<std::string>() : std::string();
    }

    std::optional<std::int64_t> integer_field(const json &object, std::string_view name)
    {
        const auto *value = field(object, name);
        if (value == nullptr || !value->is_number_integer()) {
            return std::nullopt;
        }
        return value->get<std::int64_t>();
    }

    bool valid_public_key(const std::uint8_t *key, std::size_t length)
    {
        if (length != 65 || key[0] != 0x04) {
            return false;
        }
        mbedtls_ecp_group group;
        mbedtls_ecp_point point;
        mbedtls_ecp_group_init(&group);
        mbedtls_ecp_point_init(&point);
        // The on-curve check is what stops a crafted key from steering the agreement.
        const bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0
            && mbedtls_ecp_point_read_binary(&group, &point, key, length) == 0 && mbedtls_ecp_check_pubkey(&group, &point) == 0;
        mbedtls_ecp_point_free(&point);
        mbedtls_ecp_group_free(&group);
        return ok;
    }

    bool private_matches_public(const PrivateKey &privateKey, const PublicKey &publicKey)
    {
        mbedtls_ecp_keypair pair;
        mbedtls_ecp_keypair_init(&pair);
        std::uint8_t computed[65] {};
        std::size_t length = 0;
        const bool ok = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &pair, privateKey.data(), privateKey.size()) == 0
            && mbedtls_ecp_keypair_calc_public(&pair, random_bytes, nullptr) == 0
            && mbedtls_ecp_write_public_key(&pair, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, computed, sizeof(computed)) == 0
            && length == publicKey.size() && std::memcmp(computed, publicKey.data(), length) == 0;
        mbedtls_ecp_keypair_free(&pair);
        return ok;
    }

    // PeerIdentity.DeriveSharedKey: SHA-256(PairSalt || Z), where PairSalt is
    // SHA-256("cubeshelf-friend-v1" || lower key || higher key) and Z the ECDH x-coordinate.
    bool derive_pairwise(const Profile &me, const PublicKey &peer, std::uint8_t out[32])
    {
        mbedtls_ecp_group group;
        mbedtls_ecp_point point;
        mbedtls_mpi secret, shared;
        mbedtls_ecp_group_init(&group);
        mbedtls_ecp_point_init(&point);
        mbedtls_mpi_init(&secret);
        mbedtls_mpi_init(&shared);

        std::uint8_t z[32] {};
        bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0
            && mbedtls_ecp_point_read_binary(&group, &point, peer.data(), peer.size()) == 0
            && mbedtls_ecp_check_pubkey(&group, &point) == 0
            && mbedtls_mpi_read_binary(&secret, me.privateKey.data(), me.privateKey.size()) == 0
            && mbedtls_ecdh_compute_shared(&group, &shared, &point, &secret, random_bytes, nullptr) == 0
            && mbedtls_mpi_write_binary(&shared, z, sizeof(z)) == 0;

        if (ok) {
            const bool meFirst = std::lexicographical_compare(me.publicKey.begin(), me.publicKey.end(), peer.begin(), peer.end())
                || me.publicKey == peer;
            const PublicKey &first = meFirst ? me.publicKey : peer;
            const PublicKey &second = meFirst ? peer : me.publicKey;

            Bytes saltInput(bytes_of(kPairContext), bytes_of(kPairContext) + kPairContext.size());
            saltInput.insert(saltInput.end(), first.begin(), first.end());
            saltInput.insert(saltInput.end(), second.begin(), second.end());
            std::uint8_t salt[32];
            sha256(saltInput.data(), saltInput.size(), salt);

            std::uint8_t keyInput[64];
            std::memcpy(keyInput, salt, 32);
            std::memcpy(keyInput + 32, z, 32);
            sha256(keyInput, sizeof(keyInput), out);
            mbedtls_platform_zeroize(keyInput, sizeof(keyInput));
        }

        mbedtls_platform_zeroize(z, sizeof(z));
        mbedtls_mpi_free(&shared);
        mbedtls_mpi_free(&secret);
        mbedtls_ecp_point_free(&point);
        mbedtls_ecp_group_free(&group);
        return ok;
    }

    // Decodes one UTF-8 sequence; malformed input yields U+FFFD and advances one byte.
    char32_t next_codepoint(std::string_view text, std::size_t &i)
    {
        const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
        const unsigned char lead = byte(i);
        if (lead < 0x80) {
            ++i;
            return lead;
        }
        const int extra = (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : 0;
        if (extra == 0 || i + extra >= text.size()) {
            ++i;
            return U'\uFFFD';
        }
        char32_t cp = lead & (0x3F >> extra);
        for (int k = 1; k <= extra; ++k) {
            if ((byte(i + k) & 0xC0) != 0x80) {
                ++i;
                return U'\uFFFD';
            }
            cp = (cp << 6) | (byte(i + k) & 0x3F);
        }
        i += extra + 1;
        return cp;
    }

    void append_utf8(std::string &out, char32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // char.IsWhiteSpace
    bool is_space(char32_t cp)
    {
        return cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || cp == 0x85 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A)
            || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
    }

    // char.IsControl
    bool is_control(char32_t cp)
    {
        return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
    }

    std::string tag_digits(const PublicKey &key, bool longTag)
    {
        static constexpr char label[] = "CubeShelf/tag/v1"; // followed by one NUL byte
        Bytes input(label, label + sizeof(label)); // sizeof includes the NUL
        input.insert(input.end(), key.begin(), key.end());
        std::uint8_t digest[32];
        sha256(input.data(), input.size(), digest);
        const std::uint32_t value = (std::uint32_t(digest[0]) << 24) | (std::uint32_t(digest[1]) << 16) | (std::uint32_t(digest[2]) << 8) | digest[3];
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%04u", static_cast<unsigned>(value % 10000));
        std::string out = buffer;
        if (longTag) {
            const unsigned extra = ((unsigned(digest[4]) << 8) | digest[5]) % 100;
            std::snprintf(buffer, sizeof(buffer), "%02u", extra);
            out += buffer;
        }
        return out;
    }

    // days_from_civil, Howard Hinnant's algorithm.
    std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    }

    json profile_to_json(const Profile &profile)
    {
        json friends = json::array();
        for (const auto &f : profile.friends) {
            friends.push_back({ { "k", f.publicKey }, { "n", f.name }, { "u", f.presenceUrl }, { "s", f.lastSequence }, { "p", f.paused } });
        }
        return {
            { "v", 1 },
            { "name", profile.name },
            { "d", encode_base64(profile.privateKey.data(), profile.privateKey.size()) },
            { "q", encode_base64(profile.publicKey.data(), profile.publicKey.size()) },
            { "at", profile.exportedAtUnix },
            { "friends", std::move(friends) },
        };
    }

    // The transfer document and the stored profile share one shape. "at" is a timestamp string
    // from CubeShelf and a number once stored.
    std::optional<Profile> profile_from_json(const json &document, ImportError &error)
    {
        const auto *version = field(document, "v");
        if (version == nullptr || !version->is_number_integer() || version->get<int>() != 1) {
            error = ImportError::Unsupported;
            return std::nullopt;
        }

        Profile profile;
        profile.name = sanitize_name(string_field(document, "name"));
        const auto *d = field(document, "d");
        const auto *q = field(document, "q");
        const auto scalar = d != nullptr ? decode_base64_exact(*d, profile.privateKey.size()) : std::nullopt;
        const auto publicKey = q != nullptr ? decode_base64_exact(*q, profile.publicKey.size()) : std::nullopt;
        if (!scalar || !publicKey || !valid_public_key(publicKey->data(), publicKey->size())) {
            error = ImportError::BadKey;
            return std::nullopt;
        }
        std::copy(scalar->begin(), scalar->end(), profile.privateKey.begin());
        std::copy(publicKey->begin(), publicKey->end(), profile.publicKey.begin());
        if (!private_matches_public(profile.privateKey, profile.publicKey)) {
            error = ImportError::BadKey;
            return std::nullopt;
        }

        if (const auto *at = field(document, "at")) {
            if (at->is_number_integer()) {
                profile.exportedAtUnix = at->get<std::int64_t>();
            }
            else if (at->is_string()) {
                profile.exportedAtUnix = parse_timestamp(at->get<std::string>()).value_or(0);
            }
        }

        if (const auto *friends = field(document, "friends"); friends != nullptr && friends->is_array()) {
            for (const auto &entry : *friends) {
                Friend f;
                f.publicKey = string_field(entry, "k");
                f.name = sanitize_name(string_field(entry, "n"));
                f.presenceUrl = string_field(entry, "u");
                f.lastSequence = integer_field(entry, "s").value_or(0);
                const auto *paused = field(entry, "p");
                f.paused = paused != nullptr && paused->is_boolean() && paused->get<bool>();
                PublicKey key;
                // friends.json is a file a person can edit: the scheme is checked again here.
                if (!decode_public_key(f.publicKey, key) || !f.presenceUrl.starts_with("https://") || key == profile.publicKey) {
                    continue;
                }
                profile.friends.push_back(std::move(f));
            }
        }
        error = ImportError::None;
        return profile;
    }

} // namespace

std::string encode_base64(const std::uint8_t *data, std::size_t length)
{
    std::size_t needed = 0;
    mbedtls_base64_encode(nullptr, 0, &needed, data, length);
    std::string out(needed, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out.data()), out.size(), &needed, data, length) != 0) {
        return {};
    }
    out.resize(needed);
    return out;
}

bool decode_public_key(std::string_view base64, PublicKey &out)
{
    const auto decoded = decode_base64(base64);
    if (!decoded || !valid_public_key(decoded->data(), decoded->size())) {
        return false;
    }
    std::copy(decoded->begin(), decoded->end(), out.begin());
    return true;
}

std::optional<Profile> import_profile(std::string_view text, std::string_view passphrase, ImportError &error)
{
    std::string cleaned;
    cleaned.reserve(std::min(text.size(), kMaximumProfileText));
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            cleaned += c;
        }
        if (cleaned.size() > kMaximumProfileText) {
            error = ImportError::NotAProfile;
            return std::nullopt;
        }
    }
    if (!cleaned.starts_with(kProfilePrefix)) {
        error = ImportError::NotAProfile;
        return std::nullopt;
    }

    std::string body = cleaned.substr(kProfilePrefix.size());
    for (char &c : body) {
        if (c == '-') {
            c = '+';
        }
        else if (c == '_') {
            c = '/';
        }
    }
    body.append((4 - body.size() % 4) % 4, '=');
    const auto framed = decode_base64(body);
    if (!framed) {
        error = ImportError::Unreadable;
        return std::nullopt;
    }
    if (framed->size() <= kSaltLength + kNonceLength + kTagLength) {
        error = ImportError::Unreadable;
        return std::nullopt;
    }

    const std::uint8_t *salt = framed->data();
    const std::uint8_t *nonce = salt + kSaltLength;
    const std::uint8_t *cipher = nonce + kNonceLength;
    const std::size_t cipherLength = framed->size() - kSaltLength - kNonceLength - kTagLength;
    const std::uint8_t *tag = cipher + cipherLength;

    Wiped key(kKeyLength);
    if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, bytes_of(passphrase), passphrase.size(), salt, kSaltLength, kProfileIterations,
            static_cast<std::uint32_t>(key.size()), key.data())
        != 0) {
        error = ImportError::Unreadable;
        return std::nullopt;
    }

    Wiped plain(cipherLength);
    if (!gcm_decrypt(key.data(), nonce, cipher, cipherLength, tag, bytes_of(kProfileAad), kProfileAad.size(), plain.data())) {
        error = ImportError::WrongPassphrase;
        return std::nullopt;
    }

    const json document = json::parse(plain.bytes.begin(), plain.bytes.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = ImportError::Unsupported;
        return std::nullopt;
    }
    return profile_from_json(document, error);
}

std::optional<Profile> import_profile_document(std::string_view text, ImportError &error)
{
    error = ImportError::None;
    if (text.size() > kMaximumProfileText) {
        error = ImportError::NotAProfile;
        return std::nullopt;
    }
    const json document = json::parse(text.begin(), text.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = ImportError::Unreadable;
        return std::nullopt;
    }
    return profile_from_json(document, error);
}

const char *import_error_message(ImportError error) noexcept
{
    switch (error) {
        case ImportError::None:
            return "Profile imported.";
        case ImportError::NotAProfile:
            return "This is not a profile exported by CubeShelf.";
        case ImportError::WrongPassphrase:
            return "Wrong passphrase, or the file was modified.";
        case ImportError::Unsupported:
            return "This profile comes from a CubeShelf version this Party Board does not read.";
        case ImportError::BadKey:
            return "The key in this profile is unreadable.";
        case ImportError::Unreadable:
        default:
            return "The profile is unreadable: the file was modified or cut short.";
    }
}

std::string serialize_profile(const Profile &profile)
{
    return profile_to_json(profile).dump(2);
}

std::optional<Profile> deserialize_profile(std::string_view text)
{
    const json document = json::parse(text.begin(), text.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::nullopt;
    }
    ImportError error;
    return profile_from_json(document, error);
}

std::optional<Snapshot> open_presence(const Profile &me, const PublicKey &author, std::string_view envelopeJson)
{
    const json envelope = json::parse(envelopeJson.begin(), envelopeJson.end(), nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) {
        return std::nullopt;
    }
    const auto version = integer_field(envelope, "v");
    const auto *boxes = field(envelope, "b");
    if (!version || *version != 1 || boxes == nullptr || !boxes->is_array() || boxes->empty() || boxes->size() > kMaximumBoxes) {
        return std::nullopt;
    }

    const auto *nonceField = field(envelope, "n");
    const auto *tagField = field(envelope, "t");
    const auto *payloadField = field(envelope, "p");
    const auto nonce = nonceField != nullptr ? decode_base64_exact(*nonceField, kNonceLength) : std::nullopt;
    const auto tag = tagField != nullptr ? decode_base64_exact(*tagField, kTagLength) : std::nullopt;
    const auto payload = payloadField != nullptr && payloadField->is_string() ? decode_base64(payloadField->get_ref<const std::string &>()) : std::nullopt;
    if (!nonce || !tag || !payload || payload->empty() || payload->size() > kMaximumPayload) {
        return std::nullopt;
    }

    Wiped pairwise(kKeyLength);
    if (!derive_pairwise(me, author, pairwise.data())) {
        return std::nullopt;
    }

    std::uint8_t hint[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), pairwise.data(), pairwise.size(), bytes_of(kHintContext), kHintContext.size(), hint);

    Bytes aad(bytes_of(kPresenceAad), bytes_of(kPresenceAad) + kPresenceAad.size());
    aad.insert(aad.end(), author.begin(), author.end());

    for (const auto &box : *boxes) {
        const auto *h = field(box, "h");
        const auto boxHint = h != nullptr ? decode_base64_exact(*h, kHintLength) : std::nullopt;
        if (!boxHint || !constant_time_equal(boxHint->data(), hint, kHintLength)) {
            continue;
        }
        const auto *n = field(box, "n");
        const auto *k = field(box, "k");
        const auto *t = field(box, "t");
        const auto boxNonce = n != nullptr ? decode_base64_exact(*n, kNonceLength) : std::nullopt;
        const auto wrapped = k != nullptr ? decode_base64_exact(*k, kKeyLength) : std::nullopt;
        const auto boxTag = t != nullptr ? decode_base64_exact(*t, kTagLength) : std::nullopt;
        if (!boxNonce || !wrapped || !boxTag) {
            continue;
        }

        // As in SealedPresence.TryOpen: the first box whose hint is ours decides.
        Wiped contentKey(kKeyLength);
        if (!gcm_decrypt(pairwise.data(), boxNonce->data(), wrapped->data(), wrapped->size(), boxTag->data(), aad.data(), aad.size(), contentKey.data())) {
            return std::nullopt;
        }
        Wiped plain(payload->size());
        if (!gcm_decrypt(contentKey.data(), nonce->data(), payload->data(), payload->size(), tag->data(), aad.data(), aad.size(), plain.data())) {
            return std::nullopt;
        }

        const json document = json::parse(plain.bytes.begin(), plain.bytes.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object() || integer_field(document, "Version").value_or(0) != 1) {
            return std::nullopt;
        }

        Snapshot snapshot;
        snapshot.displayName = sanitize_name(string_field(document, "DisplayName"));
        const auto published = parse_timestamp(string_field(document, "PublishedAt"));
        const auto sequence = integer_field(document, "Sequence");
        if (!published || !sequence) {
            return std::nullopt;
        }
        snapshot.publishedUnix = *published;
        snapshot.sequence = *sequence;
        switch (integer_field(document, "Status").value_or(0)) {
            case 1:
                snapshot.status = Status::Online;
                break;
            case 2:
                snapshot.status = Status::InGame;
                break;
            default:
                snapshot.status = Status::Offline;
                break;
        }
        snapshot.currentGameTitle = string_field(document, "CurrentGameTitle");

        if (const auto *invite = field(document, "Invite"); invite != nullptr && invite->is_object()) {
            Invite parsed;
            parsed.gameId = string_field(*invite, "GameId");
            parsed.gameTitle = string_field(*invite, "GameTitle");
            parsed.joinPayload = string_field(*invite, "JoinPayload");
            parsed.forFriend = string_field(*invite, "ForFriend");
            const auto expires = parse_timestamp(string_field(*invite, "ExpiresAt"));
            if (expires && !parsed.joinPayload.empty() && parsed.joinPayload.size() <= 2048) {
                parsed.expiresUnix = *expires;
                snapshot.invite = std::move(parsed);
            }
        }
        return snapshot;
    }
    return std::nullopt;
}

Status effective_status(const Snapshot &snapshot, std::int64_t nowUnix) noexcept
{
    const bool fresh = snapshot.publishedUnix <= nowUnix + kFutureSkewSeconds && nowUnix - snapshot.publishedUnix <= kFreshnessSeconds;
    return fresh ? snapshot.status : Status::Offline;
}

bool invites(const Snapshot &snapshot, const Profile &me, std::string_view gameId, std::int64_t nowUnix)
{
    if (!snapshot.invite) {
        return false;
    }
    const auto &invite = *snapshot.invite;
    const bool sameGame = invite.gameId.size() == gameId.size()
        && std::equal(invite.gameId.begin(), invite.gameId.end(), gameId.begin(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return sameGame && nowUnix < invite.expiresUnix
        && (invite.forFriend.empty() || invite.forFriend == encode_base64(me.publicKey.data(), me.publicKey.size()));
}

std::string sanitize_name(std::string_view untrusted)
{
    // Keep, then collapse whitespace runs to one space and trim, exactly like PeerName.Sanitize.
    std::vector<char32_t> kept;
    for (std::size_t i = 0; i < untrusted.size();) {
        const char32_t cp = next_codepoint(untrusted, i);
        // Controls go entirely -- a tab joins the words around it rather than separating them.
        if (is_control(cp) || cp == '#' || cp == '<' || cp == '>') {
            continue;
        }
        kept.push_back(cp);
    }

    std::vector<char32_t> collapsed;
    bool pendingSpace = false;
    for (const char32_t cp : kept) {
        if (is_space(cp)) {
            pendingSpace = !collapsed.empty();
            continue;
        }
        if (pendingSpace) {
            collapsed.push_back(' ');
            pendingSpace = false;
        }
        collapsed.push_back(cp);
    }

    // The limit is in UTF-16 units, and a surrogate pair is never cut in half.
    std::string out;
    std::size_t units = 0;
    for (const char32_t cp : collapsed) {
        const std::size_t width = cp >= 0x10000 ? 2 : 1;
        if (units + width > kMaximumNameUnits) {
            break;
        }
        units += width;
        append_utf8(out, cp);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string handle(std::string_view name, const PublicKey &key, bool longTag)
{
    return sanitize_name(name) + "#" + tag_digits(key, longTag);
}

std::optional<std::int64_t> parse_timestamp(std::string_view text)
{
    // yyyy-MM-ddTHH:mm:ss[.fffffff](Z|+HH:mm|-HH:mm)
    const auto digits = [&](std::size_t at, std::size_t count) -> std::optional<int> {
        if (at + count > text.size()) {
            return std::nullopt;
        }
        int value = 0;
        for (std::size_t i = at; i < at + count; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
                return std::nullopt;
            }
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto year = digits(0, 4), month = digits(5, 2), day = digits(8, 2), hour = digits(11, 2), minute = digits(14, 2), second = digits(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59
        || *second > 60) {
        return std::nullopt;
    }
    std::size_t at = 19;
    if (at < text.size() && text[at] == '.') {
        ++at;
        const std::size_t start = at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
            ++at;
        }
        if (at == start) {
            return std::nullopt;
        }
    }
    std::int64_t offset = 0;
    if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) {
        ++at;
    }
    else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        const auto oh = digits(at + 1, 2), om = digits(at + 4, 2);
        if (!oh || !om || at + 3 >= text.size() || text[at + 3] != ':') {
            return std::nullopt;
        }
        offset = (text[at] == '+' ? 1 : -1) * (*oh * 3600 + *om * 60);
        at += 6;
    }
    else {
        return std::nullopt;
    }
    if (at != text.size()) {
        return std::nullopt;
    }
    return days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)) * 86400 + *hour * 3600 + *minute * 60 + *second
        - offset;
}

} // namespace partyboard::online::cubeshelf
