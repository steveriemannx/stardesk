#include "TcpServer.h"

namespace sdk {

bool TcpServer::Start(uint16_t port, AcceptHandler handler)
{
    if (m_running) {
        return false;
    }
    m_listenFd = Socket::Listen(port);
    if (m_listenFd == Socket::kInvalid) {
        return false;
    }
    m_port = port;
    m_handler = std::move(handler);
    m_running = true;
    m_thread = std::thread(&TcpServer::AcceptLoop, this);
    return true;
}

void TcpServer::Stop()
{
    // Always join the accept thread (it may still be finishing even when
    // m_running is already false); std::thread destructor aborts otherwise.
    const bool wasRunning = m_running.exchange(false);
    if (wasRunning) {
        // wake the blocked accept() without releasing the fd number; the
        // accept loop closes the listener after it exits
        Socket::Shutdown(m_listenFd);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    Socket::Close(m_listenFd);
    m_listenFd = Socket::kInvalid;
    m_handler = nullptr;
}

void TcpServer::AcceptLoop()
{
    // poll instead of blocking in accept(): shutdown() cannot wake a blocked
    // accept() on a listening socket (it only affects connected sockets), so
    // Stop() would deadlock on join(). A 100ms select loop keeps Stop() fast.
    while (m_running) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET((unsigned)m_listenFd, &set);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        const int r = select((int)m_listenFd + 1, &set, nullptr, nullptr, &tv);
        if (r <= 0) {
            continue; // timeout or error: re-check m_running
        }
        std::string ip;
        uint16_t port = 0;
        const Socket::sock_t fd = Socket::Accept(m_listenFd, ip, port);
        if (fd == Socket::kInvalid) {
            continue;
        }
        if (m_handler) {
            m_handler(fd, ip);
        }
        else {
            Socket::Close(fd);
        }
    }
    Socket::Close(m_listenFd); // owned by this thread after Start()
}

} // namespace sdk
