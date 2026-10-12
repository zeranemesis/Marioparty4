#include "port/online/cubeshelf_mesh.hpp"

#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace partyboard::online::cubeshelf::mesh {
namespace {

    // ------------------------------------------------------------------ small helpers

    std::int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::int64_t unix_now()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    const std::uint8_t *bytes_of(std::string_view text)
    {
        return reinterpret_cast<const std::uint8_t *>(text.data());
    }

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
            static constexpr unsigned char personal[] = "partyboard-cubeshelf-mesh";
            if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, personal, sizeof(personal) - 1) != 0) {
                return -1;
            }
            seeded = true;
        }
        return mbedtls_ctr_drbg_random(&drbg, output, length);
    }

    template <typename T> T random_value()
    {
        T value {};
        random_bytes(nullptr, reinterpret_cast<unsigned char *>(&value), sizeof(value));
        return value;
    }

    void put16(Bytes &out, std::uint16_t v)
    {
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v));
    }

    void put32(Bytes &out, std::uint32_t v)
    {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void put64(Bytes &out, std::uint64_t v)
    {
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void put32_at(std::uint8_t *out, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
    }

    void put64_at(std::uint8_t *out, std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
    }

    std::uint32_t get32(const std::uint8_t *in)
    {
        return (std::uint32_t(in[0]) << 24) | (std::uint32_t(in[1]) << 16) | (std::uint32_t(in[2]) << 8) | in[3];
    }

    std::uint64_t get64(const std::uint8_t *in)
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | in[i];
        return v;
    }

    void append(Bytes &out, const std::uint8_t *data, std::size_t length)
    {
        out.insert(out.end(), data, data + length);
    }

    // MeshReader: reads that never run past the end; one failure fails everything after it.
    struct Reader {
        const std::uint8_t *data;
        std::size_t length;
        std::size_t offset = 0;
        bool ok = true;

        Reader(const std::uint8_t *d, std::size_t l) : data(d), length(l) {}

        const std::uint8_t *take(std::size_t count)
        {
            if (!ok || count > length - offset) {
                ok = false;
                return nullptr;
            }
            const std::uint8_t *at = data + offset;
            offset += count;
            return at;
        }
        std::uint8_t u8()
        {
            const auto *p = take(1);
            return p != nullptr ? p[0] : 0;
        }
        std::uint16_t u16()
        {
            const auto *p = take(2);
            return p != nullptr ? static_cast<std::uint16_t>((p[0] << 8) | p[1]) : 0;
        }
        std::uint32_t u32()
        {
            const auto *p = take(4);
            return p != nullptr ? get32(p) : 0;
        }
        std::uint64_t u64()
        {
            const auto *p = take(8);
            return p != nullptr ? get64(p) : 0;
        }
        std::optional<Endpoint> endpoint()
        {
            const std::uint8_t family = u8();
            Endpoint e;
            if (family == 4) {
                const auto *p = take(4);
                if (p != nullptr) std::memcpy(e.address.data(), p, 4);
            }
            else if (family == 6) {
                e.v6 = true;
                const auto *p = take(16);
                if (p != nullptr) std::memcpy(e.address.data(), p, 16);
            }
            else {
                ok = false;
            }
            e.port = u16();
            if (!ok || e.port == 0) {
                ok = false;
                return std::nullopt;
            }
            return e;
        }
        std::optional<Endpoint> optional_endpoint()
        {
            if (ok && offset < length && data[offset] == 0) {
                ++offset;
                return std::nullopt;
            }
            return endpoint();
        }
        bool done() const { return ok && offset == length; }
    };

    void put_endpoint(Bytes &out, const Endpoint &e)
    {
        if (e.v6) {
            out.push_back(6);
            append(out, e.address.data(), 16);
        }
        else {
            out.push_back(4);
            append(out, e.address.data(), 4);
        }
        put16(out, e.port);
    }

    // An IPv4 address carried as IPv6 (::ffff:a.b.c.d) is the IPv4 address.
    Endpoint normalize(Endpoint e)
    {
        static constexpr std::uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (e.v6 && std::memcmp(e.address.data(), mapped, 12) == 0) {
            std::array<std::uint8_t, 16> v4 {};
            std::memcpy(v4.data(), e.address.data() + 12, 4);
            e.address = v4;
            e.v6 = false;
        }
        return e;
    }

    std::string neighbourhood(const Endpoint &e)
    {
        std::ostringstream out;
        if (!e.v6) {
            out << int(e.address[0]) << '.' << int(e.address[1]) << '.' << int(e.address[2]);
        }
        else {
            static constexpr char hex[] = "0123456789ABCDEF";
            for (int i = 0; i < 6; ++i) out << hex[e.address[i] >> 4] << hex[e.address[i] & 15];
        }
        return out.str();
    }

    // ------------------------------------------------------------------ the curve

    struct Group {
        mbedtls_ecp_group group;
        Group()
        {
            mbedtls_ecp_group_init(&group);
            mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
        }
        ~Group() { mbedtls_ecp_group_free(&group); }
    };

    struct Mpi {
        mbedtls_mpi v;
        Mpi() { mbedtls_mpi_init(&v); }
        ~Mpi() { mbedtls_mpi_free(&v); }
    };

    struct Point {
        mbedtls_ecp_point v;
        Point() { mbedtls_ecp_point_init(&v); }
        ~Point() { mbedtls_ecp_point_free(&v); }
    };

    bool read_point(Group &g, Point &p, const std::uint8_t *key, std::size_t length)
    {
        return length == 65 && key[0] == 0x04 && mbedtls_ecp_point_read_binary(&g.group, &p.v, key, length) == 0
            && mbedtls_ecp_check_pubkey(&g.group, &p.v) == 0;
    }

    bool write_point(Group &g, const Point &p, PublicKey &out)
    {
        std::size_t length = 0;
        return mbedtls_ecp_point_write_binary(&g.group, &p.v, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, out.data(), out.size()) == 0
            && length == out.size();
    }

    bool public_of(const std::array<std::uint8_t, 32> &privateKey, PublicKey &out)
    {
        Group g;
        Mpi d;
        Point q;
        return mbedtls_mpi_read_binary(&d.v, privateKey.data(), privateKey.size()) == 0
            && mbedtls_ecp_mul(&g.group, &q.v, &d.v, &g.group.G, random_bytes, nullptr) == 0 && write_point(g, q, out);
    }

    std::array<std::uint8_t, 32> new_private_key()
    {
        Group g;
        Mpi d;
        Point q;
        std::array<std::uint8_t, 32> out {};
        mbedtls_ecp_gen_keypair(&g.group, &d.v, &q.v, random_bytes, nullptr);
        mbedtls_mpi_write_binary(&d.v, out.data(), out.size());
        return out;
    }

    // NoiseHandshake's DH: ECDH, then SHA-256 of the shared X coordinate (DeriveKeyFromHash).
    // A point off the curve is refused here, which is what keeps a crafted key from steering it.
    bool dh(const std::array<std::uint8_t, 32> &own, const PublicKey &remote, std::uint8_t out[32])
    {
        Group g;
        Point q;
        Mpi d, z;
        std::uint8_t x[32] {};
        const bool ok = read_point(g, q, remote.data(), remote.size()) && mbedtls_mpi_read_binary(&d.v, own.data(), own.size()) == 0
            && mbedtls_ecdh_compute_shared(&g.group, &z.v, &q.v, &d.v, random_bytes, nullptr) == 0
            && mbedtls_mpi_write_binary(&z.v, x, sizeof(x)) == 0;
        if (ok) sha256(x, sizeof(x), out);
        mbedtls_platform_zeroize(x, sizeof(x));
        return ok;
    }

    void hmac(const std::uint8_t *key, std::size_t keyLength, const std::uint8_t *data, std::size_t length, std::uint8_t out[32])
    {
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLength, data, length, out);
    }

    bool gcm_seal(const std::uint8_t key[32], const std::uint8_t nonce[12], const std::uint8_t *plain, std::size_t length,
        const std::uint8_t *aad, std::size_t aadLength, std::uint8_t *out, std::uint8_t tag[16])
    {
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
            && mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, length, nonce, 12, aad, aadLength, plain, out, 16, tag) == 0;
        mbedtls_gcm_free(&gcm);
        return ok;
    }

    bool gcm_open(const std::uint8_t key[32], const std::uint8_t nonce[12], const std::uint8_t *cipher, std::size_t length,
        const std::uint8_t tag[16], const std::uint8_t *aad, std::size_t aadLength, std::uint8_t *out)
    {
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
            && mbedtls_gcm_auth_decrypt(&gcm, length, nonce, 12, aad, aadLength, tag, 16, cipher, out) == 0;
        mbedtls_gcm_free(&gcm);
        return ok;
    }

    void noise_nonce(std::uint64_t counter, std::uint8_t out[12])
    {
        std::memset(out, 0, 12);
        put64_at(out + 4, counter);
    }

    int leading_zero_bits(const std::uint8_t *bytes, std::size_t length)
    {
        int bits = 0;
        for (std::size_t i = 0; i < length; ++i) {
            if (bytes[i] == 0) {
                bits += 8;
                continue;
            }
            for (int bit = 7; bit >= 0; --bit) {
                if (bytes[i] & (1 << bit)) return bits;
                ++bits;
            }
        }
        return bits;
    }

    void proof_hash(std::string_view domain, const PublicKey &key, std::uint64_t nonce, std::uint8_t out[32])
    {
        Bytes buffer(bytes_of(domain), bytes_of(domain) + domain.size());
        append(buffer, key.data(), key.size());
        put64(buffer, nonce);
        sha256(buffer.data(), buffer.size(), out);
    }

    std::uint64_t solve_proof(const PublicKey &key, int difficulty)
    {
        static constexpr std::string_view domain = "cubeshelf-node-work-v1";
        Bytes buffer(bytes_of(domain), bytes_of(domain) + domain.size());
        append(buffer, key.data(), key.size());
        const std::size_t slot = buffer.size();
        buffer.resize(slot + 8);
        std::uint8_t digest[32];
        mbedtls_sha256_context context;
        mbedtls_sha256_init(&context);
        for (std::uint64_t nonce = random_value<std::uint64_t>();; ++nonce) {
            put64_at(buffer.data() + slot, nonce);
            mbedtls_sha256_starts(&context, 0);
            mbedtls_sha256_update(&context, buffer.data(), buffer.size());
            mbedtls_sha256_finish(&context, digest);
            if (leading_zero_bits(digest, 32) >= difficulty) {
                mbedtls_sha256_free(&context);
                return nonce;
            }
        }
    }

    bool closer(const NodeId &a, const NodeId &b, const NodeId &target)
    {
        for (std::size_t i = 0; i < 32; ++i) {
            const std::uint8_t x = a[i] ^ target[i];
            const std::uint8_t y = b[i] ^ target[i];
            if (x != y) return x < y;
        }
        return false;
    }

    // ------------------------------------------------------------------ the socket

    struct Socket {
#if defined(_WIN32)
        SOCKET handle = INVALID_SOCKET;
        bool valid() const { return handle != INVALID_SOCKET; }
#else
        int handle = -1;
        bool valid() const { return handle >= 0; }
#endif
        bool v6 = false;

        bool open()
        {
#if defined(_WIN32)
            static const bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            if (!started) return false;
#endif
            // Dual-stack when the system has IPv6, IPv4 only otherwise.
            handle = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
            if (valid()) {
                int off = 0;
                setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off), sizeof(off));
                sockaddr_in6 any {};
                any.sin6_family = AF_INET6;
                if (::bind(handle, reinterpret_cast<sockaddr *>(&any), sizeof(any)) == 0) {
                    v6 = true;
                    return nonblocking();
                }
                close();
            }
            handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (!valid()) return false;
            sockaddr_in any {};
            any.sin_family = AF_INET;
            if (::bind(handle, reinterpret_cast<sockaddr *>(&any), sizeof(any)) != 0) {
                close();
                return false;
            }
            return nonblocking();
        }

        bool nonblocking()
        {
#if defined(_WIN32)
            u_long on = 1;
            return ioctlsocket(handle, FIONBIO, &on) == 0;
#else
            return fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK) == 0;
#endif
        }

        void close()
        {
            if (!valid()) return;
#if defined(_WIN32)
            closesocket(handle);
            handle = INVALID_SOCKET;
#else
            ::close(handle);
            handle = -1;
#endif
        }

        void send(const std::uint8_t *data, std::size_t length, const Endpoint &to)
        {
            if (!valid()) return;
            sockaddr_storage storage {};
            socklen_t size = 0;
            if (v6) {
                auto *a = reinterpret_cast<sockaddr_in6 *>(&storage);
                a->sin6_family = AF_INET6;
                a->sin6_port = htons(to.port);
                if (to.v6) {
                    std::memcpy(&a->sin6_addr, to.address.data(), 16);
                }
                else {
                    std::uint8_t mapped[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
                    std::memcpy(mapped + 12, to.address.data(), 4);
                    std::memcpy(&a->sin6_addr, mapped, 16);
                }
                size = sizeof(sockaddr_in6);
            }
            else {
                if (to.v6) return;
                auto *a = reinterpret_cast<sockaddr_in *>(&storage);
                a->sin_family = AF_INET;
                a->sin_port = htons(to.port);
                std::memcpy(&a->sin_addr, to.address.data(), 4);
                size = sizeof(sockaddr_in);
            }
            ::sendto(handle, reinterpret_cast<const char *>(data), static_cast<int>(length), 0, reinterpret_cast<sockaddr *>(&storage), size);
        }

        // One datagram, waiting up to timeoutMs; false when nothing came.
        bool receive(Bytes &data, Endpoint &from, int timeoutMs)
        {
            if (!valid()) return false;
            fd_set read;
            FD_ZERO(&read);
            FD_SET(handle, &read);
            timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            if (::select(static_cast<int>(handle) + 1, &read, nullptr, nullptr, &tv) <= 0) return false;
            data.resize(2048);
            sockaddr_storage storage {};
            socklen_t size = sizeof(storage);
            const auto received = ::recvfrom(handle, reinterpret_cast<char *>(data.data()), static_cast<int>(data.size()), 0,
                reinterpret_cast<sockaddr *>(&storage), &size);
            if (received <= 0) return false;
            data.resize(static_cast<std::size_t>(received));
            if (storage.ss_family == AF_INET6) {
                const auto *a = reinterpret_cast<const sockaddr_in6 *>(&storage);
                from.v6 = true;
                std::memcpy(from.address.data(), &a->sin6_addr, 16);
                from.port = ntohs(a->sin6_port);
                from = normalize(from);
            }
            else {
                const auto *a = reinterpret_cast<const sockaddr_in *>(&storage);
                from = {};
                std::memcpy(from.address.data(), &a->sin_addr, 4);
                from.port = ntohs(a->sin_port);
            }
            return true;
        }
    };

} // namespace

// ------------------------------------------------------------------ endpoints

std::string Endpoint::to_string() const
{
    char text[INET6_ADDRSTRLEN] {};
    if (v6) {
        inet_ntop(AF_INET6, address.data(), text, sizeof(text));
        return "[" + std::string(text) + "]:" + std::to_string(port);
    }
    inet_ntop(AF_INET, address.data(), text, sizeof(text));
    return std::string(text) + ":" + std::to_string(port);
}

std::optional<Endpoint> Endpoint::parse(std::string_view text)
{
#if defined(_WIN32)
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
    Endpoint e;
    std::string host;
    std::string_view portText;
    if (!text.empty() && text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
        host = std::string(text.substr(1, close - 1));
        portText = text.substr(close + 2);
        e.v6 = true;
    }
    else {
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos) return std::nullopt;
        host = std::string(text.substr(0, colon));
        portText = text.substr(colon + 1);
    }
    if (portText.empty() || portText.size() > 5) return std::nullopt;
    unsigned port = 0;
    for (const char c : portText) {
        if (c < '0' || c > '9') return std::nullopt;
        port = port * 10 + unsigned(c - '0');
    }
    if (port == 0 || port > 65535) return std::nullopt;
    e.port = static_cast<std::uint16_t>(port);
    if (inet_pton(e.v6 ? AF_INET6 : AF_INET, host.c_str(), e.address.data()) != 1) return std::nullopt;
    return normalize(e);
}

bool Endpoint::operator<(const Endpoint &other) const
{
    if (v6 != other.v6) return v6 < other.v6;
    if (address != other.address) return address < other.address;
    return port < other.port;
}

bool is_public(const Endpoint &raw)
{
    const Endpoint e = normalize(raw);
    const auto &b = e.address;
    if (!e.v6) {
        const int a = b[0], c = b[1];
        return !(a == 0 || a == 10 || a == 127 || a >= 224 || (a == 100 && c >= 64 && c <= 127) || (a == 169 && c == 254)
            || (a == 172 && c >= 16 && c <= 31) || (a == 192 && c == 168) || (a == 192 && c == 0 && b[2] == 0)
            || (a == 192 && c == 0 && b[2] == 2) || (a == 198 && (c == 18 || c == 19)) || (a == 198 && c == 51 && b[2] == 100)
            || (a == 203 && c == 0 && b[2] == 113));
    }
    if ((b[0] & 0xE0) != 0x20) return false;
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8) return false;
    if (b[0] == 0x20 && b[1] == 0x02) return false;
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0) return false;
    return true;
}

// ------------------------------------------------------------------ primitives

void sha256(const std::uint8_t *data, std::size_t length, std::uint8_t out[32])
{
    mbedtls_sha256(data, length, out, 0);
}

Bytes domain_hash(std::string_view domain, std::initializer_list<std::pair<const std::uint8_t *, std::size_t>> parts)
{
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    std::uint8_t length[4];
    put32_at(length, static_cast<std::uint32_t>(domain.size()));
    mbedtls_sha256_update(&context, length, 4);
    mbedtls_sha256_update(&context, bytes_of(domain), domain.size());
    for (const auto &[data, size] : parts) {
        put32_at(length, static_cast<std::uint32_t>(size));
        mbedtls_sha256_update(&context, length, 4);
        if (size > 0) mbedtls_sha256_update(&context, data, size);
    }
    Bytes out(32);
    mbedtls_sha256_finish(&context, out.data());
    mbedtls_sha256_free(&context);
    return out;
}

Bytes hkdf(const std::uint8_t *secret, std::size_t secretLength, std::string_view info, std::size_t length)
{
    Bytes out(length);
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), nullptr, 0, secret, secretLength, bytes_of(info), info.size(), out.data(), length);
    return out;
}

bool is_on_curve(const PublicKey &key)
{
    Group g;
    Point p;
    return read_point(g, p, key.data(), key.size());
}

std::optional<PublicKey> derived_public_key(const std::uint8_t *secret, std::size_t secretLength, std::string_view label)
{
    // MeshCrypto.DeriveSigningKey: 48 bytes of HKDF, reduced modulo n-1, plus one.
    Bytes material = hkdf(secret, secretLength, "cubeshelf-mesh-signing-key-v1|" + std::string(label), 48);
    Group g;
    Mpi value, order, scalar;
    Point q;
    PublicKey out {};
    const bool ok = mbedtls_mpi_read_binary(&value.v, material.data(), material.size()) == 0
        && mbedtls_mpi_sub_int(&order.v, &g.group.N, 1) == 0 && mbedtls_mpi_mod_mpi(&scalar.v, &value.v, &order.v) == 0
        && mbedtls_mpi_add_int(&scalar.v, &scalar.v, 1) == 0
        && mbedtls_ecp_mul(&g.group, &q.v, &scalar.v, &g.group.G, random_bytes, nullptr) == 0 && write_point(g, q, out);
    mbedtls_platform_zeroize(material.data(), material.size());
    if (!ok) return std::nullopt;
    return out;
}

NodeId locator_of(const PublicKey &signer)
{
    const Bytes hash = domain_hash("cubeshelf-locator-v1", { { signer.data(), signer.size() } });
    NodeId id {};
    std::memcpy(id.data(), hash.data(), 32);
    return id;
}

NodeId node_id(const PublicKey &key, std::uint64_t nonce)
{
    NodeId id {};
    proof_hash("cubeshelf-node-id-v1", key, nonce, id.data());
    return id;
}

int proof_bits(const PublicKey &key, std::uint64_t nonce)
{
    std::uint8_t digest[32];
    proof_hash("cubeshelf-node-work-v1", key, nonce, digest);
    return leading_zero_bits(digest, 32);
}

std::optional<Bytes> open_sealed(const std::uint8_t key[32], const Bytes &sealed, const std::uint8_t *aad, std::size_t aadLength)
{
    if (sealed.size() < 12 + 16) return std::nullopt;
    Bytes plain(sealed.size() - 28);
    if (!gcm_open(key, sealed.data(), sealed.data() + 12, plain.size(), sealed.data() + sealed.size() - 16, aad, aadLength, plain.data())) {
        return std::nullopt;
    }
    return plain;
}

// ------------------------------------------------------------------ records

std::optional<Record> read_record(const std::uint8_t *data, std::size_t length, std::size_t &used)
{
    Reader r(data, length);
    Record record;
    if (const auto *p = r.take(32)) std::memcpy(record.locator.data(), p, 32);
    record.kind = r.u8();
    if (const auto *p = r.take(65)) std::memcpy(record.signer.data(), p, 65);
    record.sequence = r.u64();
    record.expiresUnix = static_cast<std::int64_t>(r.u64());
    const std::uint32_t payloadLength = r.u32();
    if (payloadLength > 96 * 1024) r.ok = false;
    if (const auto *p = r.take(payloadLength)) record.payload.assign(p, p + payloadLength);
    if (const auto *p = r.take(64)) std::memcpy(record.signature.data(), p, 64);
    if (!r.ok || (record.kind != 1 && record.kind != 2) || record.expiresUnix < 0) return std::nullopt;
    used = r.offset;
    return record;
}

bool record_valid(const Record &record, std::int64_t nowUnix)
{
    // MeshRecord.IsValid, for the one kind a reader needs: signed.
    if (record.kind != 1) return false;
    if (record.expiresUnix <= nowUnix || record.expiresUnix > nowUnix + 24 * 3600 + 300) return false;
    if (record.payload.size() > 96 * 1024 || record.signer[0] != 0x04 || record.locator != locator_of(record.signer)) return false;

    std::uint8_t sequence[8], expires[8];
    put64_at(sequence, record.sequence);
    put64_at(expires, static_cast<std::uint64_t>(record.expiresUnix));
    const Bytes signedBytes = domain_hash("cubeshelf-record-v1",
        { { record.locator.data(), 32 }, { &record.kind, 1 }, { record.signer.data(), 65 }, { sequence, 8 }, { expires, 8 },
            { record.payload.data(), record.payload.size() } });
    // ECDsa.VerifyData hashes the data once more.
    std::uint8_t digest[32];
    sha256(signedBytes.data(), signedBytes.size(), digest);

    Group g;
    Point q;
    Mpi r, s;
    return read_point(g, q, record.signer.data(), record.signer.size()) && mbedtls_mpi_read_binary(&r.v, record.signature.data(), 32) == 0
        && mbedtls_mpi_read_binary(&s.v, record.signature.data() + 32, 32) == 0
        && mbedtls_ecdsa_verify(&g.group, digest, sizeof(digest), &q.v, &r.v, &s.v) == 0;
}

NodeId pointer_locator(const std::uint8_t pairKey[32], const PublicKey &owner, std::int64_t day)
{
    std::uint8_t ownerHash[32];
    sha256(owner.data(), owner.size(), ownerHash);
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string label = "pointer|";
    for (int i = 0; i < 16; ++i) {
        label += hex[ownerHash[i] >> 4];
        label += hex[ownerHash[i] & 15];
    }
    label += "|" + std::to_string(day);
    const auto key = derived_public_key(pairKey, 32, label);
    return key ? locator_of(*key) : NodeId {};
}

std::optional<NodeId> open_pointer(const std::uint8_t pairKey[32], const NodeId &pointerLocator, const Bytes &payload)
{
    Bytes key = hkdf(pairKey, 32, "cubeshelf-mesh-pointer-v1", 32);
    auto opened = open_sealed(key.data(), payload, pointerLocator.data(), pointerLocator.size());
    mbedtls_platform_zeroize(key.data(), key.size());
    if (!opened || opened->size() != 32) return std::nullopt;
    NodeId id {};
    std::memcpy(id.data(), opened->data(), 32);
    return id;
}

// ------------------------------------------------------------------ Noise XX, initiator

struct NoiseInitiator::Impl {
    std::array<std::uint8_t, 32> staticPrivate {};
    PublicKey staticPublic {};
    std::array<std::uint8_t, 32> ephemeralPrivate {};
    PublicKey ephemeralPublic {};
    PublicKey remoteEphemeral {};
    std::array<std::uint8_t, 32> ck {};
    std::array<std::uint8_t, 32> k {};
    bool hasKey = false;
    std::uint64_t n = 0;
    bool failed = false;
};

namespace {
    void mix_hash(std::array<std::uint8_t, 32> &h, const std::uint8_t *data, std::size_t length)
    {
        Bytes buffer(h.begin(), h.end());
        append(buffer, data, length);
        sha256(buffer.data(), buffer.size(), h.data());
    }

    void hkdf2(const std::uint8_t ck[32], const std::uint8_t *ikm, std::size_t ikmLength, std::uint8_t first[32], std::uint8_t second[32])
    {
        std::uint8_t temp[32];
        hmac(ck, 32, ikm, ikmLength, temp);
        const std::uint8_t one = 1;
        hmac(temp, 32, &one, 1, first);
        std::uint8_t input[33];
        std::memcpy(input, first, 32);
        input[32] = 2;
        hmac(temp, 32, input, 33, second);
        mbedtls_platform_zeroize(temp, sizeof(temp));
    }
}

NoiseInitiator::NoiseInitiator(const std::array<std::uint8_t, 32> &staticPrivate, const Bytes &prologue,
    const std::optional<std::array<std::uint8_t, 32>> &ephemeralPrivate)
    : impl_(std::make_unique<Impl>())
{
    impl_->staticPrivate = staticPrivate;
    public_of(staticPrivate, impl_->staticPublic);
    impl_->ephemeralPrivate = ephemeralPrivate ? *ephemeralPrivate : new_private_key();
    public_of(impl_->ephemeralPrivate, impl_->ephemeralPublic);
    static constexpr std::string_view name = "Noise_XX_P256_AESGCM_SHA256";
    h_.fill(0);
    std::memcpy(h_.data(), name.data(), name.size());
    impl_->ck = h_;
    mix_hash(h_, prologue.data(), prologue.size());
}

NoiseInitiator::~NoiseInitiator()
{
    mbedtls_platform_zeroize(impl_->staticPrivate.data(), 32);
    mbedtls_platform_zeroize(impl_->ephemeralPrivate.data(), 32);
    mbedtls_platform_zeroize(impl_->ck.data(), 32);
    mbedtls_platform_zeroize(impl_->k.data(), 32);
}

Bytes NoiseInitiator::write_message1(const Bytes &payload)
{
    Bytes message(impl_->ephemeralPublic.begin(), impl_->ephemeralPublic.end());
    mix_hash(h_, impl_->ephemeralPublic.data(), 65);
    // No key yet: the payload travels in clear, and is hashed.
    append(message, payload.data(), payload.size());
    mix_hash(h_, payload.data(), payload.size());
    return message;
}

namespace {
    bool mix_key(NoiseInitiator::Impl &s, const std::uint8_t ikm[32])
    {
        std::uint8_t ck[32], k[32];
        hkdf2(s.ck.data(), ikm, 32, ck, k);
        std::memcpy(s.ck.data(), ck, 32);
        std::memcpy(s.k.data(), k, 32);
        mbedtls_platform_zeroize(ck, 32);
        mbedtls_platform_zeroize(k, 32);
        s.hasKey = true;
        s.n = 0;
        return true;
    }
}

std::optional<Bytes> NoiseInitiator::read_message2(const std::uint8_t *message, std::size_t length)
{
    auto &s = *impl_;
    if (s.failed || length < 65 + 65 + 16 + 16) {
        s.failed = true;
        return std::nullopt;
    }
    std::memcpy(s.remoteEphemeral.data(), message, 65);
    if (s.remoteEphemeral[0] != 0x04) {
        s.failed = true;
        return std::nullopt;
    }
    mix_hash(h_, message, 65);
    std::uint8_t shared[32];
    if (!dh(s.ephemeralPrivate, s.remoteEphemeral, shared)) {
        s.failed = true;
        return std::nullopt;
    }
    mix_key(s, shared);

    // Decrypt-and-hash, twice: the responder's static key, then its payload.
    const auto decrypt = [&](const std::uint8_t *cipher, std::size_t cipherLength) -> std::optional<Bytes> {
        if (cipherLength < 16) return std::nullopt;
        Bytes plain(cipherLength - 16);
        std::uint8_t nonce[12];
        noise_nonce(s.n, nonce);
        if (!gcm_open(s.k.data(), nonce, cipher, plain.size(), cipher + plain.size(), h_.data(), 32, plain.data())) return std::nullopt;
        ++s.n;
        mix_hash(h_, cipher, cipherLength);
        return plain;
    };

    const auto remoteStatic = decrypt(message + 65, 65 + 16);
    if (!remoteStatic || remoteStatic->size() != 65 || (*remoteStatic)[0] != 0x04) {
        s.failed = true;
        return std::nullopt;
    }
    std::memcpy(remoteStatic_.data(), remoteStatic->data(), 65);
    if (!dh(s.ephemeralPrivate, remoteStatic_, shared)) {
        s.failed = true;
        return std::nullopt;
    }
    mix_key(s, shared);
    mbedtls_platform_zeroize(shared, sizeof(shared));

    auto payload = decrypt(message + 65 + 65 + 16, length - (65 + 65 + 16));
    if (!payload) s.failed = true;
    return payload;
}

Bytes NoiseInitiator::write_message3(const Bytes &payload)
{
    auto &s = *impl_;
    Bytes message;
    const auto encrypt = [&](const std::uint8_t *plain, std::size_t plainLength) {
        const std::size_t start = message.size();
        message.resize(start + plainLength + 16);
        std::uint8_t nonce[12];
        noise_nonce(s.n++, nonce);
        gcm_seal(s.k.data(), nonce, plain, plainLength, h_.data(), 32, message.data() + start, message.data() + start + plainLength);
        mix_hash(h_, message.data() + start, plainLength + 16);
    };
    encrypt(s.staticPublic.data(), 65);
    std::uint8_t shared[32];
    dh(s.staticPrivate, s.remoteEphemeral, shared);
    mix_key(s, shared);
    mbedtls_platform_zeroize(shared, sizeof(shared));
    encrypt(payload.data(), payload.size());
    return message;
}

std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 32>> NoiseInitiator::split() const
{
    std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 32>> keys;
    hkdf2(impl_->ck.data(), nullptr, 0, keys.first.data(), keys.second.data());
    return keys;
}

// ------------------------------------------------------------------ the client

namespace {

    constexpr std::uint8_t kMagic = 0xC5;
    constexpr std::uint8_t kVersion = 1;
    constexpr std::size_t kInitLength = 256;
    constexpr std::size_t kCookieLength = 16;
    constexpr std::size_t kHeaderLength = 14;
    constexpr std::size_t kInitPayloadLength = kInitLength - 2 - 4 - kCookieLength - 65;
    constexpr std::size_t kFragmentPayload = 1232 - kHeaderLength - 16 - 7;
    constexpr std::size_t kMaximumMessage = 160 * 1024;
    constexpr int kK = 16;
    constexpr int kAlpha = 3;
    constexpr std::int64_t kRequestTimeoutMs = 1500;
    constexpr std::int64_t kHandshakeTimeoutMs = 3000;
    constexpr std::uint8_t kFindNode = 0x02;
    constexpr std::uint8_t kFindValue = 0x03;

    enum : std::uint8_t { TypeInit = 1, TypeResponse = 2, TypeFinish = 3, TypeTransport = 4, TypeCookie = 5 };
    enum : std::uint8_t { FrameFragment = 1, FrameKeepalive = 2, FrameNack = 3, FrameClose = 4 };

    struct Contact {
        PublicKey key {};
        std::uint64_t nonce = 0;
        Endpoint endpoint;
        NodeId id {};
        int failures = 0;
    };

    struct Reassembly {
        int count = 0;
        std::vector<std::optional<Bytes>> chunks;
        int received = 0;
        std::size_t bytes = 0;
        std::int64_t createdAt = 0;
        std::int64_t lastFragmentAt = 0;
        int nacks = 0;
    };

    struct Session {
        std::uint32_t localIndex = 0;
        std::uint32_t remoteIndex = 0;
        Endpoint endpoint;
        std::array<std::uint8_t, 32> sendKey {};
        std::array<std::uint8_t, 32> receiveKey {};
        std::uint64_t sendCounter = 0;
        std::uint64_t highest = 0;
        bool receivedAny = false;
        std::array<std::uint64_t, 16> window {};
        PublicKey remoteKey {};
        std::uint64_t remoteNonce = 0;
        NodeId remoteId {};
        std::uint8_t remoteFlags = 0;
        bool confirmed = false;
        Bytes pendingFinish;
        std::int64_t establishedAt = 0;
        std::int64_t lastReceivedAt = 0;
        std::map<std::uint32_t, Reassembly> incoming;
        std::deque<std::uint32_t> recentlyCompleted;
        std::uint32_t nextMessageId = 0;
        bool closed = false;
    };

    struct Pending {
        std::uint32_t localIndex = 0;
        Endpoint endpoint;
        std::unique_ptr<NoiseInitiator> handshake;
        Bytes message1;
        std::array<std::uint8_t, kCookieLength> cookie {};
        std::int64_t startedAt = 0;
        int sends = 0;
        bool failed = false;
    };

    struct Request {
        std::uint32_t session = 0;
        std::uint32_t id = 0;
        std::uint8_t op = 0;
        Bytes body;
        std::int64_t sentAt = 0;
        int attempts = 0;
        std::optional<Bytes> reply;
        bool failed = false;
    };

    // One query of a lookup: reach a node, check it is who we were told, ask it.
    struct Query {
        Contact contact;
        int state = 0; // 0 connecting, 1 asking, 2 answered, 3 failed
        std::uint32_t requestKey = 0;
        Bytes reply;
    };

} // namespace

struct Client::Impl {
    ClientOptions options;
    Socket socket;
    std::array<std::uint8_t, 32> staticPrivate {};
    PublicKey staticPublic {};
    std::uint64_t nonce = 0;
    NodeId localId {};
    bool started = false;

    std::map<std::uint32_t, Session> sessions;
    std::map<std::uint32_t, Pending> pending;
    std::map<std::uint32_t, Request> requests;
    std::uint32_t nextRequestKey = 1;
    std::uint32_t nextRequestId = 1;

    std::map<NodeId, Contact> contacts;
    std::set<Endpoint> seeds;
    std::map<Endpoint, std::int64_t> unreachable;
    std::map<std::string, std::pair<std::int64_t, NodeId>> mainLocators;

    std::uint32_t new_index()
    {
        for (;;) {
            const auto index = random_value<std::uint32_t>();
            if (index != 0 && !sessions.count(index) && !pending.count(index)) return index;
        }
    }

    // ---------------------------------------------------------------- sessions

    Session *session_at(const Endpoint &endpoint)
    {
        for (auto &[index, session] : sessions) {
            if (!session.closed && session.endpoint == endpoint && now_ms() - session.establishedAt < 30 * 60 * 1000) return &session;
        }
        return nullptr;
    }

    Pending *pending_at(const Endpoint &endpoint)
    {
        for (auto &[index, p] : pending) {
            if (!p.failed && p.endpoint == endpoint) return &p;
        }
        return nullptr;
    }

    Bytes build_init(const Pending &p)
    {
        Bytes packet(kInitLength, 0);
        packet[0] = kMagic;
        packet[1] = TypeInit;
        put32_at(packet.data() + 2, p.localIndex);
        std::memcpy(packet.data() + 6, p.cookie.data(), kCookieLength);
        std::memcpy(packet.data() + 6 + kCookieLength, p.message1.data(), p.message1.size());
        return packet;
    }

    void connect(const Endpoint &endpoint)
    {
        if (session_at(endpoint) != nullptr || pending_at(endpoint) != nullptr) return;
        Pending p;
        p.localIndex = new_index();
        p.endpoint = endpoint;
        static const Bytes prologue(bytes_of("cubeshelf-mesh-v1"), bytes_of("cubeshelf-mesh-v1") + 17);
        p.handshake = std::make_unique<NoiseInitiator>(staticPrivate, prologue);
        Bytes payload(kInitPayloadLength, 0);
        payload[0] = kVersion;
        p.message1 = p.handshake->write_message1(payload);
        p.startedAt = now_ms();
        p.sends = 1;
        const Bytes packet = build_init(p);
        socket.send(packet.data(), packet.size(), endpoint);
        pending[p.localIndex] = std::move(p);
    }

    // Our proof, our flags (none: we serve nothing), how the other side looks from here, no addresses.
    Bytes our_payload(const Endpoint &observed) const
    {
        Bytes out;
        out.push_back(kVersion);
        put64(out, nonce);
        out.push_back(0);
        put_endpoint(out, observed);
        out.push_back(0);
        return out;
    }

    void on_response(const std::uint8_t *packet, std::size_t length, const Endpoint &from)
    {
        if (length < 10 + 65 * 2) return;
        const std::uint32_t responderIndex = get32(packet + 2);
        const std::uint32_t ourIndex = get32(packet + 6);
        auto it = pending.find(ourIndex);
        if (it == pending.end() || it->second.failed || !(it->second.endpoint == from)) return;
        Pending p = std::move(it->second);
        pending.erase(it);

        const auto payload = p.handshake->read_message2(packet + 10, length - 10);
        if (!payload) return;
        // The responder's proof of work: no proof, no session.
        Reader r(payload->data(), payload->size());
        const std::uint8_t version = r.u8();
        const std::uint64_t remoteNonce = r.u64();
        const std::uint8_t flags = r.u8() & 0x03;
        (void)r.optional_endpoint();
        const int count = r.u8();
        if (count > 2) return;
        for (int i = 0; i < count; ++i) (void)r.endpoint();
        const PublicKey remote = p.handshake->remote_static();
        if (!r.done() || version != kVersion || proof_bits(remote, remoteNonce) < options.proofDifficulty || !is_on_curve(remote)) return;

        const Bytes finish = p.handshake->write_message3(our_payload(from));
        Bytes finishPacket { kMagic, TypeFinish };
        put32(finishPacket, responderIndex);
        append(finishPacket, finish.data(), finish.size());

        Session session;
        session.localIndex = p.localIndex;
        session.remoteIndex = responderIndex;
        session.endpoint = from;
        std::tie(session.sendKey, session.receiveKey) = p.handshake->split();
        session.remoteKey = remote;
        session.remoteNonce = remoteNonce;
        session.remoteId = node_id(remote, remoteNonce);
        session.remoteFlags = flags;
        session.pendingFinish = finishPacket;
        session.establishedAt = session.lastReceivedAt = now_ms();
        if (session.remoteId == localId) return;
        socket.send(finishPacket.data(), finishPacket.size(), from);

        // A reachable node we dialled ourselves has proven its address.
        if ((flags & 1) != 0 && options.routable(from)) {
            Contact c { remote, remoteNonce, from, session.remoteId, 0 };
            contacts[c.id] = c;
        }
        sessions[session.localIndex] = std::move(session);
    }

    void on_cookie(const std::uint8_t *packet, std::size_t length, const Endpoint &from)
    {
        if (length != 2 + 4 + kCookieLength) return;
        auto it = pending.find(get32(packet + 2));
        if (it == pending.end() || !(it->second.endpoint == from)) return;
        std::memcpy(it->second.cookie.data(), packet + 6, kCookieLength);
        const Bytes init = build_init(it->second);
        socket.send(init.data(), init.size(), from);
    }

    void send_frame(Session &session, const Bytes &frame)
    {
        if (session.closed) return;
        Bytes packet(kHeaderLength + frame.size() + 16);
        packet[0] = kMagic;
        packet[1] = TypeTransport;
        put32_at(packet.data() + 2, session.remoteIndex);
        const std::uint64_t counter = session.sendCounter++;
        put64_at(packet.data() + 6, counter);
        std::uint8_t iv[12];
        noise_nonce(counter, iv);
        gcm_seal(session.sendKey.data(), iv, frame.data(), frame.size(), packet.data(), kHeaderLength, packet.data() + kHeaderLength,
            packet.data() + kHeaderLength + frame.size());
        socket.send(packet.data(), packet.size(), session.endpoint);
    }

    void send_message(Session &session, std::uint8_t op, std::uint32_t requestId, const Bytes &body)
    {
        Bytes message { op };
        put32(message, requestId);
        append(message, body.data(), body.size());
        const std::size_t count = std::max<std::size_t>(1, (message.size() + kFragmentPayload - 1) / kFragmentPayload);
        const std::uint32_t messageId = session.nextMessageId++;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t offset = i * kFragmentPayload;
            const std::size_t take = std::min(kFragmentPayload, message.size() - offset);
            Bytes frame { FrameFragment };
            put32(frame, messageId);
            frame.push_back(static_cast<std::uint8_t>(i));
            frame.push_back(static_cast<std::uint8_t>(count));
            append(frame, message.data() + offset, take);
            send_frame(session, frame);
        }
    }

    bool window_allows(const Session &s, std::uint64_t counter) const
    {
        if (!s.receivedAny || counter > s.highest) return true;
        if (s.highest - counter >= 1024) return false;
        const std::size_t bit = counter % 1024;
        return (s.window[bit / 64] & (1ULL << (bit % 64))) == 0;
    }

    void window_accept(Session &s, std::uint64_t counter)
    {
        if (!s.receivedAny || counter > s.highest) {
            const std::uint64_t start = s.receivedAny ? s.highest + 1 : 0;
            if (counter - start >= 1024) {
                s.window.fill(0);
            }
            else {
                for (std::uint64_t skipped = start; skipped < counter; ++skipped) {
                    const std::size_t b = skipped % 1024;
                    s.window[b / 64] &= ~(1ULL << (b % 64));
                }
            }
            s.highest = counter;
            s.receivedAny = true;
        }
        const std::size_t bit = counter % 1024;
        s.window[bit / 64] |= 1ULL << (bit % 64);
    }

    void on_transport(const std::uint8_t *packet, std::size_t length, const Endpoint &from)
    {
        if (length < kHeaderLength + 16) return;
        auto it = sessions.find(get32(packet + 2));
        if (it == sessions.end() || it->second.closed || !(it->second.endpoint == from)) return;
        Session &s = it->second;
        const std::uint64_t counter = get64(packet + 6);
        if (!window_allows(s, counter)) return;
        Bytes frame(length - kHeaderLength - 16);
        std::uint8_t iv[12];
        noise_nonce(counter, iv);
        if (!gcm_open(s.receiveKey.data(), iv, packet + kHeaderLength, frame.size(), packet + length - 16, packet, kHeaderLength, frame.data())) return;
        window_accept(s, counter);
        s.lastReceivedAt = now_ms();
        if (!s.confirmed) {
            s.confirmed = true;
            s.pendingFinish.clear();
        }
        if (frame.empty()) return;
        switch (frame[0]) {
        case FrameKeepalive:
            if (frame.size() == 2 && frame[1] == 1) send_frame(s, Bytes { FrameKeepalive, 0 });
            break;
        case FrameClose:
            close(s);
            break;
        case FrameFragment:
            on_fragment(s, frame);
            break;
        default:
            break;
        }
    }

    void on_fragment(Session &s, const Bytes &frame)
    {
        if (frame.size() < 7) return;
        const std::uint32_t messageId = get32(frame.data() + 1);
        const int index = frame[5];
        const int count = frame[6];
        const std::size_t dataLength = frame.size() - 7;
        if (count == 0 || index >= count || dataLength > kFragmentPayload) return;
        if (std::size_t(count) * kFragmentPayload > kMaximumMessage + kFragmentPayload) return;
        if (std::find(s.recentlyCompleted.begin(), s.recentlyCompleted.end(), messageId) != s.recentlyCompleted.end()) return;

        Bytes complete;
        if (count == 1) {
            complete.assign(frame.begin() + 7, frame.end());
        }
        else {
            auto &a = s.incoming[messageId];
            if (a.count == 0) {
                if (s.incoming.size() > 8) {
                    s.incoming.erase(s.incoming.begin());
                    return;
                }
                a.count = count;
                a.chunks.resize(count);
                a.createdAt = now_ms();
            }
            if (a.count != count) return;
            a.lastFragmentAt = now_ms();
            if (!a.chunks[index]) {
                a.chunks[index] = Bytes(frame.begin() + 7, frame.end());
                ++a.received;
                a.bytes += dataLength;
            }
            if (a.bytes > kMaximumMessage) {
                s.incoming.erase(messageId);
                return;
            }
            if (a.received < a.count) return;
            for (const auto &chunk : a.chunks) append(complete, chunk->data(), chunk->size());
            s.incoming.erase(messageId);
        }
        s.recentlyCompleted.push_back(messageId);
        while (s.recentlyCompleted.size() > 128) s.recentlyCompleted.pop_front();
        deliver(s, complete);
    }

    void deliver(Session &s, const Bytes &message)
    {
        if (message.size() < 5) return;
        const std::uint8_t op = message[0];
        const std::uint32_t requestId = get32(message.data() + 1);
        // Only answers matter here: we serve nothing, so a request to us goes unanswered.
        if ((op & 0x80) == 0) return;
        for (auto &[key, request] : requests) {
            if (request.session == s.localIndex && request.id == requestId && !request.reply && !request.failed) {
                request.reply = Bytes(message.begin() + 5, message.end());
            }
        }
    }

    void close(Session &s)
    {
        s.closed = true;
        for (auto &[key, request] : requests) {
            if (request.session == s.localIndex && !request.reply) request.failed = true;
        }
    }

    // ---------------------------------------------------------------- the loop

    void process(const Bytes &packet, const Endpoint &from)
    {
        if (packet.size() < 2 || packet[0] != kMagic || packet.size() > 1232 + 64) return;
        switch (packet[1]) {
        case TypeResponse:
            on_response(packet.data(), packet.size(), from);
            break;
        case TypeTransport:
            on_transport(packet.data(), packet.size(), from);
            break;
        case TypeCookie:
            on_cookie(packet.data(), packet.size(), from);
            break;
        default:
            break; // handshakes towards us, probes, punches: a client takes none
        }
    }

    void maintain()
    {
        const std::int64_t now = now_ms();
        for (auto it = pending.begin(); it != pending.end();) {
            auto &p = it->second;
            const std::int64_t age = now - p.startedAt;
            if (age > kHandshakeTimeoutMs) {
                unreachable[p.endpoint] = now + 10 * 60 * 1000;
                it = pending.erase(it);
                continue;
            }
            const std::int64_t due = p.sends == 1 ? 400 : p.sends == 2 ? 1000 : p.sends == 3 ? 2000 : INT64_MAX;
            if (age >= due) {
                ++p.sends;
                const Bytes init = build_init(p);
                socket.send(init.data(), init.size(), p.endpoint);
            }
            ++it;
        }

        for (auto &[key, request] : requests) {
            if (request.reply || request.failed) continue;
            auto it = sessions.find(request.session);
            if (it == sessions.end() || it->second.closed) {
                request.failed = true;
                continue;
            }
            if (now - request.sentAt < kRequestTimeoutMs) continue;
            if (request.attempts >= 2) {
                request.failed = true;
                continue;
            }
            // Requests are all safe to repeat: the one retry, with our last handshake message
            // again if nothing has confirmed it arrived.
            if (!it->second.confirmed && !it->second.pendingFinish.empty())
                socket.send(it->second.pendingFinish.data(), it->second.pendingFinish.size(), it->second.endpoint);
            send_message(it->second, request.op, request.id, request.body);
            request.sentAt = now;
            ++request.attempts;
        }

        for (auto &[index, s] : sessions) {
            if (s.closed) continue;
            for (auto it = s.incoming.begin(); it != s.incoming.end();) {
                auto &a = it->second;
                if (now - a.createdAt > 8000 || a.nacks >= 4) {
                    if (now - a.lastFragmentAt > 1000) {
                        it = s.incoming.erase(it);
                        continue;
                    }
                    ++it;
                    continue;
                }
                if (now - a.lastFragmentAt >= 250) {
                    Bytes frame { FrameNack };
                    put32(frame, it->first);
                    std::vector<std::uint8_t> missing;
                    for (int i = 0; i < a.count && missing.size() < 200; ++i) {
                        if (!a.chunks[i]) missing.push_back(static_cast<std::uint8_t>(i));
                    }
                    frame.push_back(static_cast<std::uint8_t>(missing.size()));
                    append(frame, missing.data(), missing.size());
                    ++a.nacks;
                    a.lastFragmentAt = now;
                    send_frame(s, frame);
                }
                ++it;
            }
        }

        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second.closed || now - it->second.lastReceivedAt > 3 * 60 * 1000) {
                it = sessions.erase(it);
                continue;
            }
            ++it;
        }
    }

    void pump(int timeoutMs)
    {
        Bytes packet;
        Endpoint from;
        const std::int64_t until = now_ms() + timeoutMs;
        int wait = timeoutMs;
        while (socket.receive(packet, from, wait)) {
            process(packet, from);
            wait = 0;
            if (now_ms() >= until) break;
        }
        maintain();
    }

    std::uint32_t ask(Session &s, std::uint8_t op, const Bytes &body)
    {
        Request r;
        r.session = s.localIndex;
        r.id = nextRequestId++;
        if (nextRequestId == 0) nextRequestId = 1;
        r.op = op;
        r.body = body;
        r.sentAt = now_ms();
        r.attempts = 1;
        const std::uint32_t key = nextRequestKey++;
        requests[key] = std::move(r);
        send_message(s, op, requests[key].id, body);
        return key;
    }

    // ---------------------------------------------------------------- lookups

    std::vector<Contact> closest(const NodeId &target, std::size_t count) const
    {
        std::vector<Contact> all;
        for (const auto &[id, c] : contacts) all.push_back(c);
        std::sort(all.begin(), all.end(), [&](const Contact &a, const Contact &b) { return closer(a.id, b.id, target); });
        if (all.size() > count) all.resize(count);
        return all;
    }

    std::vector<Contact> read_contacts(Reader &r)
    {
        std::vector<Contact> out;
        const int count = r.u8();
        if (count > kK * 2) return out;
        std::map<std::string, int> neighbourhoods;
        for (int i = 0; i < count; ++i) {
            Contact c;
            const auto *key = r.take(65);
            if (key == nullptr) break;
            std::memcpy(c.key.data(), key, 65);
            c.nonce = r.u64();
            auto endpoint = r.endpoint();
            if (!r.ok || !endpoint) break;
            c.endpoint = normalize(*endpoint);
            if (proof_bits(c.key, c.nonce) < options.proofDifficulty || !is_on_curve(c.key)) break;
            c.id = node_id(c.key, c.nonce);
            // Only public addresses, and one answer cannot steer a lookup into one neighbourhood.
            if (c.id == localId || !options.routable(c.endpoint)) continue;
            auto &seen = neighbourhoods[neighbourhood(c.endpoint)];
            if (seen >= 2) continue;
            ++seen;
            out.push_back(c);
        }
        return out;
    }

    // Kademlia's iterative lookup (MeshDht.LookupAsync), as a loop over the socket.
    std::vector<Record> lookup(const NodeId &target, bool wantValue)
    {
        struct Candidate {
            Contact contact;
            int state = 0; // 0 waiting, 1 asked, 2 answered, 3 failed
            std::optional<std::uint32_t> request;
        };
        std::map<NodeId, Candidate> candidates;
        for (const auto &c : closest(target, kK)) candidates[c.id] = Candidate { c };
        std::vector<Record> records;
        const std::int64_t deadline = now_ms() + options.lookupSeconds * 1000;
        const std::uint8_t op = wantValue ? kFindValue : kFindNode;
        const Bytes body(target.begin(), target.end());

        const auto window = [&] {
            std::vector<Candidate *> out;
            for (auto &[id, c] : candidates) {
                if (c.state != 3) out.push_back(&c);
            }
            std::sort(out.begin(), out.end(), [&](const Candidate *a, const Candidate *b) { return closer(a->contact.id, b->contact.id, target); });
            if (out.size() > std::size_t(kK)) out.resize(kK);
            return out;
        };

        while (now_ms() < deadline) {
            auto w = window();
            if (!w.empty() && std::all_of(w.begin(), w.end(), [](const Candidate *c) { return c->state == 2; })) break;

            int inFlight = 0;
            for (auto &[id, c] : candidates) inFlight += c.state == 1 ? 1 : 0;
            for (Candidate *c : w) {
                if (inFlight >= kAlpha) break;
                if (c->state != 0) continue;
                const auto u = unreachable.find(c->contact.endpoint);
                if (u != unreachable.end() && u->second > now_ms()) {
                    c->state = 3;
                    continue;
                }
                c->state = 1;
                connect(c->contact.endpoint);
                ++inFlight;
            }
            if (inFlight == 0) break;

            pump(50);

            for (auto &[id, c] : candidates) {
                if (c.state != 1) continue;
                if (!c.request) {
                    Session *s = session_at(c.contact.endpoint);
                    if (s != nullptr) {
                        // Whoever answers at that address has to be the node we were told about.
                        if (s->remoteId != c.contact.id) {
                            c.state = 3;
                            continue;
                        }
                        c.request = ask(*s, op, body);
                    }
                    else if (pending_at(c.contact.endpoint) == nullptr) {
                        c.state = 3;
                        contacts.erase(c.contact.id);
                    }
                    continue;
                }
                auto &request = requests[*c.request];
                if (request.failed) {
                    c.state = 3;
                    requests.erase(*c.request);
                    continue;
                }
                if (!request.reply) continue;
                const Bytes reply = std::move(*request.reply);
                requests.erase(*c.request);
                c.state = 2;
                contacts[c.contact.id] = c.contact;

                Reader r(reply.data(), reply.size());
                if (wantValue && r.u8() == 1) {
                    const int count = std::min<int>(r.u8(), 32);
                    const std::int64_t unixNow = unix_now();
                    for (int i = 0; i < count && r.ok; ++i) {
                        std::size_t used = 0;
                        auto record = read_record(reply.data() + r.offset, reply.size() - r.offset, used);
                        if (!record) break;
                        r.offset += used;
                        // Only what genuinely sits at this locator and verifies counts.
                        if (record->locator == target && record_valid(*record, unixNow)) records.push_back(std::move(*record));
                    }
                    continue;
                }
                for (const auto &found : read_contacts(r)) {
                    if (!candidates.count(found.id)) candidates[found.id] = Candidate { found };
                }
            }
        }

        for (auto &[id, c] : candidates) {
            if (c.request) requests.erase(*c.request);
        }
        return records;
    }

    void save_nodes() const
    {
        if (options.nodesFile.empty() || contacts.empty()) return;
        std::ofstream out(options.nodesFile, std::ios::trunc);
        int written = 0;
        for (const auto &[id, c] : contacts) {
            if (written++ >= 64) break;
            out << encode_base64(c.key.data(), c.key.size()) << ' ' << c.nonce << ' ' << c.endpoint.to_string() << '\n';
        }
    }

    void load_nodes()
    {
        if (options.nodesFile.empty()) return;
        std::ifstream in(options.nodesFile);
        std::string key, nonceText, endpointText;
        while (in >> key >> nonceText >> endpointText) {
            Contact c;
            auto endpoint = Endpoint::parse(endpointText);
            if (!decode_public_key(key, c.key) || !endpoint || !options.routable(*endpoint)) continue;
            try {
                c.nonce = std::stoull(nonceText);
            }
            catch (...) {
                continue;
            }
            if (proof_bits(c.key, c.nonce) < options.proofDifficulty) continue;
            c.endpoint = *endpoint;
            c.id = node_id(c.key, c.nonce);
            contacts[c.id] = c;
        }
    }
};

Client::Client(ClientOptions options) : impl_(std::make_unique<Impl>())
{
    impl_->options = std::move(options);
}

Client::~Client()
{
    if (impl_->started) {
        impl_->save_nodes();
        impl_->socket.close();
    }
    mbedtls_platform_zeroize(impl_->staticPrivate.data(), 32);
}

bool Client::start()
{
    auto &s = *impl_;
    if (s.started) return true;
    if (!s.socket.open()) return false;
    // The node key: kept for a day at most, then a new one -- the id is not tied to anyone, and
    // changing it daily keeps it from following the phone around for longer than that.
    bool reused = false;
    const std::int64_t now = unix_now();
    if (!s.options.nodeKeyFile.empty()) {
        std::ifstream in(s.options.nodeKeyFile);
        std::string keyText;
        std::uint64_t nonce = 0;
        std::int64_t createdAt = 0;
        if (in >> keyText >> nonce >> createdAt && keyText.size() == 64 && now - createdAt >= 0 && now - createdAt < 24 * 3600) {
            for (std::size_t i = 0; i < 32; ++i) s.staticPrivate[i] = static_cast<std::uint8_t>(std::stoi(keyText.substr(i * 2, 2), nullptr, 16));
            if (public_of(s.staticPrivate, s.staticPublic) && proof_bits(s.staticPublic, nonce) >= s.options.proofDifficulty) {
                s.nonce = nonce;
                reused = true;
            }
        }
    }
    if (!reused) {
        s.staticPrivate = new_private_key();
        public_of(s.staticPrivate, s.staticPublic);
        s.nonce = solve_proof(s.staticPublic, s.options.proofDifficulty);
        if (!s.options.nodeKeyFile.empty()) {
            static constexpr char hex[] = "0123456789abcdef";
            std::string keyText;
            for (const auto b : s.staticPrivate) {
                keyText += hex[b >> 4];
                keyText += hex[b & 15];
            }
            std::ofstream out(s.options.nodeKeyFile, std::ios::trunc);
            out << keyText << ' ' << s.nonce << ' ' << now << '\n';
        }
    }
    s.localId = node_id(s.staticPublic, s.nonce);
    s.load_nodes();
    s.started = true;
    return true;
}

void Client::add_seeds(const std::vector<Endpoint> &seeds)
{
    for (const auto &seed : seeds) {
        const Endpoint e = normalize(seed);
        if (impl_->options.routable(e) && impl_->seeds.size() < 64) impl_->seeds.insert(e);
    }
}

std::size_t Client::known_nodes() const
{
    return impl_->contacts.size();
}

void Client::join()
{
    auto &s = *impl_;
    if (!s.started || s.contacts.size() >= std::size_t(kK / 2)) return;
    int asked = 0;
    for (const auto &seed : s.seeds) {
        if (asked++ >= 32) break;
        s.connect(seed);
    }
    const std::int64_t deadline = now_ms() + kHandshakeTimeoutMs + 500;
    while (now_ms() < deadline && !s.pending.empty()) s.pump(50);
    // Then a look around our own id, which is what fills the list of nodes.
    if (!s.contacts.empty()) s.lookup(s.localId, false);
    s.save_nodes();
}

std::vector<Record> Client::find_value(const NodeId &locator)
{
    return impl_->started ? impl_->lookup(locator, true) : std::vector<Record> {};
}

NetworkRead Client::read_presence(const Profile &me, const PublicKey &friendKey, std::int64_t nowUnix)
{
    auto &s = *impl_;
    NetworkRead out;
    if (!s.started) return out;
    if (s.contacts.empty()) join();
    if (s.contacts.empty()) return out;
    out.reachable = true;

    std::uint8_t pair[32];
    if (!pair_key(me, friendKey, pair)) return out;
    const std::string friendText = encode_base64(friendKey.data(), friendKey.size());
    const std::int64_t today = nowUnix / 86400;
    for (const std::int64_t day : { today, today - 1 }) {
        std::optional<NodeId> main;
        const auto cached = s.mainLocators.find(friendText);
        if (cached != s.mainLocators.end() && cached->second.first == day) {
            main = cached->second.second;
        }
        else {
            const NodeId pointer = pointer_locator(pair, friendKey, day);
            const auto pointers = s.lookup(pointer, true);
            if (pointers.empty()) continue;
            main = open_pointer(pair, pointer, pointers.front().payload);
        }
        if (!main) continue;

        auto records = s.lookup(*main, true);
        if (records.empty()) {
            s.mainLocators.erase(friendText);
            continue;
        }
        // A holder serving an old copy is outvoted by the others.
        const auto best = std::max_element(records.begin(), records.end(), [](const Record &a, const Record &b) { return a.sequence < b.sequence; });
        s.mainLocators[friendText] = { day, *main };
        out.envelope = std::string(best->payload.begin(), best->payload.end());
        out.sequence = best->sequence;
        break;
    }
    mbedtls_platform_zeroize(pair, sizeof(pair));
    s.save_nodes();
    return out;
}

// ------------------------------------------------------------------ the meeting point

namespace {

    // Bencode, strictly and boundedly (Bencode.cs): everything here comes from strangers.
    struct BValue {
        enum Kind { Integer, String, List, Dictionary } kind = Integer;
        std::int64_t integer = 0;
        std::string string;
        std::vector<BValue> list;
        std::vector<std::pair<std::string, BValue>> dictionary;

        const BValue *get(std::string_view key) const
        {
            for (const auto &[k, v] : dictionary) {
                if (k == key) return &v;
            }
            return nullptr;
        }
    };

    struct BReader {
        const std::uint8_t *data;
        std::size_t length;
        std::size_t offset = 0;
        int items = 0;

        bool value(BValue &out, int depth)
        {
            if (depth > 8 || offset >= length || ++items > 512) return false;
            const std::uint8_t first = data[offset];
            if (first == 'i') {
                ++offset;
                bool negative = false;
                if (offset < length && data[offset] == '-') {
                    negative = true;
                    ++offset;
                }
                const std::size_t start = offset;
                std::int64_t number = 0;
                while (offset < length && data[offset] >= '0' && data[offset] <= '9') {
                    if (offset - start >= 18) return false;
                    number = number * 10 + (data[offset++] - '0');
                }
                const std::size_t digits = offset - start;
                if (digits == 0 || offset >= length || data[offset] != 'e' || (data[start] == '0' && (digits > 1 || negative))) return false;
                ++offset;
                out.kind = BValue::Integer;
                out.integer = negative ? -number : number;
                return true;
            }
            if (first >= '0' && first <= '9') {
                out.kind = BValue::String;
                return string(out.string);
            }
            if (first == 'l' || first == 'd') {
                ++offset;
                out.kind = first == 'l' ? BValue::List : BValue::Dictionary;
                while (offset < length && data[offset] != 'e') {
                    if (out.kind == BValue::List) {
                        BValue item;
                        if (!value(item, depth + 1)) return false;
                        out.list.push_back(std::move(item));
                    }
                    else {
                        std::string key;
                        if (data[offset] < '0' || data[offset] > '9' || !string(key)) return false;
                        BValue item;
                        if (!value(item, depth + 1)) return false;
                        if (out.get(key) != nullptr) return false;
                        out.dictionary.emplace_back(std::move(key), std::move(item));
                    }
                }
                if (offset >= length) return false;
                ++offset;
                return true;
            }
            return false;
        }

        bool string(std::string &out)
        {
            const std::size_t start = offset;
            std::size_t size = 0;
            while (offset < length && data[offset] >= '0' && data[offset] <= '9') {
                if (offset - start >= 5) return false;
                size = size * 10 + (data[offset++] - '0');
            }
            if (offset == start || offset >= length || data[offset] != ':' || (data[start] == '0' && offset - start > 1)) return false;
            ++offset;
            if (size > length - offset) return false;
            out.assign(reinterpret_cast<const char *>(data + offset), size);
            offset += size;
            return true;
        }
    };

    void bstring(std::string &out, std::string_view text)
    {
        out += std::to_string(text.size());
        out += ':';
        out += text;
    }

    Endpoint compact(const std::uint8_t *bytes)
    {
        Endpoint e;
        std::memcpy(e.address.data(), bytes, 4);
        e.port = static_cast<std::uint16_t>((bytes[4] << 8) | bytes[5]);
        return e;
    }

} // namespace

std::vector<Endpoint> find_at_meeting_point(int budgetSeconds, std::function<bool(const Endpoint &)> routable, const std::vector<Endpoint> &startingNodes, int *answered)
{
    // MainlineRendezvous.MeetingPoint: SHA-1 of a fixed label, the same for every CubeShelf.
    static constexpr std::string_view label = "cubeshelf-mesh-rendezvous-v1";
    std::uint8_t meeting[20];
    mbedtls_sha1(bytes_of(label), label.size(), meeting);

    Socket socket;
    if (!socket.open()) return {};
    std::uint8_t id[20];
    random_bytes(nullptr, id, sizeof(id));
    const std::int64_t deadline = now_ms() + std::int64_t(budgetSeconds) * 1000;

    std::vector<Endpoint> start = startingNodes;
    if (start.empty()) {
        // The DHT's public entry points; any one answering is enough.
        static constexpr std::pair<const char *, const char *> routers[] = { { "router.bittorrent.com", "6881" }, { "dht.transmissionbt.com", "6881" },
            { "router.utorrent.com", "6881" }, { "dht.libtorrent.org", "25401" }, { "dht.aelitis.com", "6881" } };
        for (const auto &[host, port] : routers) {
            addrinfo hints {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo *result = nullptr;
            if (getaddrinfo(host, port, &hints, &result) != 0) continue;
            for (addrinfo *a = result; a != nullptr && start.size() < 10; a = a->ai_next) {
                const auto *in = reinterpret_cast<const sockaddr_in *>(a->ai_addr);
                Endpoint e;
                std::memcpy(e.address.data(), &in->sin_addr, 4);
                e.port = ntohs(in->sin_port);
                start.push_back(e);
            }
            freeaddrinfo(result);
            if (now_ms() > deadline) break;
        }
    }

    struct Candidate {
        Endpoint endpoint;
        std::array<std::uint8_t, 20> distance {};
        bool known = false; // a starting node has no distance yet
        bool asked = false;
    };
    std::map<Endpoint, Candidate> candidates;
    for (const auto &e : start) candidates[e] = Candidate { e };
    std::set<Endpoint> peers;
    std::map<std::uint16_t, Endpoint> inFlight;
    std::map<std::uint16_t, std::int64_t> sentAt;
    int queries = 0;

    const auto query = [&](const Endpoint &to) {
        std::uint16_t transaction = 0;
        do random_bytes(nullptr, reinterpret_cast<unsigned char *>(&transaction), sizeof(transaction));
        while (inFlight.count(transaction));
        // get_peers, as a read-only node (BEP 43): we ask, we are never kept in anyone's table.
        std::string message = "d1:ad2:id";
        bstring(message, std::string_view(reinterpret_cast<const char *>(id), 20));
        message += "9:info_hash";
        bstring(message, std::string_view(reinterpret_cast<const char *>(meeting), 20));
        message += "e1:q9:get_peers2:roi1e1:t";
        const char t[2] = { char(transaction >> 8), char(transaction & 0xFF) };
        bstring(message, std::string_view(t, 2));
        message += "1:y1:qe";
        socket.send(bytes_of(message), message.size(), to);
        inFlight[transaction] = to;
        sentAt[transaction] = now_ms();
        ++queries;
    };

    while (now_ms() < deadline && queries < 96 && peers.size() < 64) {
        // The nearest not yet asked, starting nodes first, eight at a time.
        std::vector<Candidate *> next;
        for (auto &[e, c] : candidates) {
            if (!c.asked) next.push_back(&c);
        }
        std::sort(next.begin(), next.end(), [](const Candidate *a, const Candidate *b) {
            if (a->known != b->known) return !a->known;
            return a->distance < b->distance;
        });
        for (Candidate *c : next) {
            if (inFlight.size() >= 8 || queries >= 96) break;
            c->asked = true;
            query(c->endpoint);
        }
        if (inFlight.empty()) break;

        Bytes packet;
        Endpoint from;
        if (socket.receive(packet, from, 100) && packet.size() <= 2048) {
            BReader reader { packet.data(), packet.size() };
            BValue message;
            if (reader.value(message, 0) && reader.offset == packet.size() && message.kind == BValue::Dictionary) {
                const BValue *t = message.get("t");
                const BValue *y = message.get("y");
                if (t != nullptr && t->kind == BValue::String && t->string.size() == 2 && y != nullptr && y->string == "r") {
                    const std::uint16_t transaction = static_cast<std::uint16_t>((std::uint8_t(t->string[0]) << 8) | std::uint8_t(t->string[1]));
                    const auto it = inFlight.find(transaction);
                    // Only the node a query went to may answer it.
                    if (it != inFlight.end() && it->second == from) {
                        inFlight.erase(it);
                        sentAt.erase(transaction);
                        if (answered != nullptr) ++*answered;
                        if (const BValue *r = message.get("r"); r != nullptr && r->kind == BValue::Dictionary) {
                            if (const BValue *values = r->get("values"); values != nullptr && values->kind == BValue::List) {
                                for (const auto &v : values->list) {
                                    if (v.kind != BValue::String || v.string.size() != 6) continue;
                                    const Endpoint peer = compact(reinterpret_cast<const std::uint8_t *>(v.string.data()));
                                    if (peer.port != 0 && routable(peer) && peers.size() < 64) peers.insert(peer);
                                }
                            }
                            if (const BValue *nodes = r->get("nodes"); nodes != nullptr && nodes->kind == BValue::String && nodes->string.size() % 26 == 0) {
                                const auto *bytes = reinterpret_cast<const std::uint8_t *>(nodes->string.data());
                                for (std::size_t offset = 0; offset + 26 <= nodes->string.size() && candidates.size() < 1024; offset += 26) {
                                    const Endpoint node = compact(bytes + offset + 20);
                                    if (node.port == 0 || !routable(node) || candidates.count(node)) continue;
                                    Candidate c { node };
                                    c.known = true;
                                    for (int i = 0; i < 20; ++i) c.distance[i] = bytes[offset + i] ^ meeting[i];
                                    candidates[node] = c;
                                }
                            }
                        }
                    }
                }
            }
        }
        // A node that has not answered in two seconds is given up on.
        for (auto it = sentAt.begin(); it != sentAt.end();) {
            if (now_ms() - it->second > 2000) {
                inFlight.erase(it->first);
                it = sentAt.erase(it);
                continue;
            }
            ++it;
        }
    }
    socket.close();
    return std::vector<Endpoint>(peers.begin(), peers.end());
}

} // namespace partyboard::online::cubeshelf::mesh
