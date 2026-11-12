#ifndef STARDESK_NET_TCP_SERVER_H_
#define STARDESK_NET_TCP_SERVER_H_

#include "Socket.h"

#include <atomic>
#include <functional>
#include <thread>

namespace sdk {

/** Simple TCP server: one accept thread, connections handed to the handler
 *  (the handler is responsible for moving the fd to its own thread).
 */
class TcpServer {
public:
    typedef std::function<void(Socket::sock_t fd, const std::string& peerIp)> AcceptHandler;

    TcpServer() = default;
    ~TcpServer() { Stop(); }
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /** Bind and start accepting; false when the port is unavailable. */
    bool Start(uint16_t port, AcceptHandler handler);
    void Stop();

    bool IsRunning() const { return m_running; }
    uint16_t Port() const { return m_port; }

private:
    void AcceptLoop();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    Socket::sock_t m_listenFd = Socket::kInvalid;
    AcceptHandler m_handler;
    uint16_t m_port = 0;
};

} // namespace sdk

#endif // STARDESK_NET_TCP_SERVER_H_
