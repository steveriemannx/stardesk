#ifndef STARDESK_NET_SOCKET_H_
#define STARDESK_NET_SOCKET_H_

#include "dui/dui.h"

#include <string>
#include <vector>

namespace sdk {

/** Minimal cross-platform socket layer (BSD sockets on POSIX, Winsock2 on
 *  Windows). Blocking I/O with select()-based timeouts; sockets are owned by
 *  the caller and closed with Close(). All functions are thread-safe as long
 *  as each fd is used by one thread at a time.
 */
class Socket {
public:
#ifdef _WIN32
    typedef unsigned long long sock_t;
#else
    typedef int sock_t;
#endif
    static const sock_t kInvalid;

    /** One-time global init (WSAStartup on Windows). Call before any socket
     *  use; Cleanup() at app exit. Safe to call multiple times.
     */
    static bool Startup();
    static void Cleanup();

    // ---- TCP ----
    static sock_t CreateTcp();
    /** Disable Nagle's algorithm (low-latency small-message stream). */
    static bool SetNoDelay(sock_t fd);
    /** Blocking connect with timeout (milliseconds). */
    static bool Connect(sock_t fd, const std::string& host, uint16_t port, int timeoutMs);
    static sock_t Listen(uint16_t port);
    /** Accept one connection; returns peer ip/port. */
    static sock_t Accept(sock_t listenFd, std::string& peerIp, uint16_t& peerPort);

    /** Send all bytes (loops on partial writes). Returns false on error/close. */
    static bool SendAll(sock_t fd, const void* data, size_t len, int timeoutMs);
    /** Receive exactly len bytes. Returns false on error/close/timeout. */
    static bool RecvExact(sock_t fd, void* data, size_t len, int timeoutMs);

    /** True when data is available to read within timeoutMs. */
    static bool WaitReadable(sock_t fd, int timeoutMs);
    /** True when the socket can be written within timeoutMs. */
    static bool WaitWritable(sock_t fd, int timeoutMs);

    static bool SetNonBlocking(sock_t fd, bool nonBlocking);
    /** Wake up blocked recv/accept without releasing the fd number (safe to
     *  call from another thread; Close() is NOT - it can race with fd reuse). */
    static void Shutdown(sock_t fd);
    static void Close(sock_t fd);

    // ---- UDP ----
    static sock_t CreateUdp(bool broadcast);
    static bool UdpSend(sock_t fd, const void* data, size_t len,
                        const std::string& host, uint16_t port);
    /** Receive one datagram; returns payload length or -1. */
    static int UdpRecv(sock_t fd, void* data, size_t len, int timeoutMs,
                       std::string& fromIp, uint16_t& fromPort);
    /** Bind a UDP socket to a port (SO_REUSEADDR enabled) for discovery. */
    static bool UdpBind(sock_t fd, uint16_t port);

    // ---- helpers ----
    /** Enumerate local IPv4 addresses (for display and beacons). */
    static std::vector<std::string> GetLocalIPv4s();
    /** Human-readable last error. */
    static std::string LastError();
};

} // namespace sdk

#endif // STARDESK_NET_SOCKET_H_
