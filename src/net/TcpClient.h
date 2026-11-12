#ifndef STARDESK_NET_TCP_CLIENT_H_
#define STARDESK_NET_TCP_CLIENT_H_

#include "Socket.h"

#include <string>

namespace sdk {

/** Client-side connection helper (the real work lives in ClientSession). */
class TcpClient {
public:
    /** Blocking connect with timeout; on success fd is connected and
     *  returned (caller owns it). Returns Socket::kInvalid on failure.
     */
    static Socket::sock_t ConnectTo(const std::string& host, uint16_t port, int timeoutMs);
};

} // namespace sdk

#endif // STARDESK_NET_TCP_CLIENT_H_
