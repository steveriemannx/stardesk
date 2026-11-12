#ifndef STARDESK_NET_SHA256_H_
#define STARDESK_NET_SHA256_H_

#include <cstdint>
#include <string>
#include <vector>

namespace sdk {

/** Minimal self-contained SHA-256 (FIPS 180-4), no external dependencies.
 *  Used for the challenge-response password check.
 */
class Sha256 {
public:
    static const size_t kDigestSize = 32;

    static void Hash(const uint8_t* data, size_t len, uint8_t digest[32]);
    static void Hash(const std::string& text, uint8_t digest[32]);
    /** Hex string (lowercase), 64 chars. */
    static std::string Hex(const uint8_t digest[32]);
};

} // namespace sdk

#endif // STARDESK_NET_SHA256_H_
