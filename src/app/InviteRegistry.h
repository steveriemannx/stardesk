#ifndef STARDESK_APP_INVITE_REGISTRY_H_
#define STARDESK_APP_INVITE_REGISTRY_H_

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sdk {

/** One-time tokens for reverse remote-assistance invitations ("请求协助").
 *  The machine that wants to be controlled creates a token, sends it to the
 *  peer and waits (TTL) for a connection carrying that token in its Hello.
 *  Consume() removes the token, so it can be used exactly once.
 */
class InviteRegistry
{
public:
    static InviteRegistry& Instance()
    {
        static InviteRegistry instance;
        return instance;
    }

    void Add(const std::string& peerIp, uint16_t port, const uint8_t token[32],
             int ttlSeconds = 120)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Prune();
        Entry entry;
        entry.ip = peerIp;
        entry.port = port;
        std::memcpy(entry.token, token, 32);
        entry.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds);
        m_entries.push_back(entry);
    }

    /** True (and the token is consumed) when peerIp presents a valid token. */
    bool Consume(const std::string& peerIp, const uint8_t token[32])
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Prune();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->ip == peerIp && std::memcmp(it->token, token, 32) == 0) {
                m_entries.erase(it);
                return true;
            }
        }
        return false;
    }

private:
    struct Entry {
        std::string ip;
        uint16_t port = 0;
        uint8_t token[32] = {0};
        std::chrono::steady_clock::time_point expiry;
    };

    void Prune()
    {
        const auto now = std::chrono::steady_clock::now();
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                       [now](const Entry& e) { return e.expiry <= now; }),
                        m_entries.end());
    }

    std::mutex m_mutex;
    std::vector<Entry> m_entries;
};

} // namespace sdk

#endif // STARDESK_APP_INVITE_REGISTRY_H_
