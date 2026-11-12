#include "Socket.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sdk {

const Socket::sock_t Socket::kInvalid =
#ifdef _WIN32
    INVALID_SOCKET;
#else
    -1;
#endif

namespace {

int LastErr()
{
#ifdef _WIN32
    return (int)WSAGetLastError();
#else
    return errno;
#endif
}

} // namespace

// ---------------------------------------------------------------- lifecycle

bool Socket::Startup()
{
#ifdef _WIN32
    static bool initialized = false;
    if (initialized) {
        return true;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    initialized = true;
#endif
    return true;
}

void Socket::Cleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

// ---------------------------------------------------------------- TCP

Socket::sock_t Socket::CreateTcp()
{
#ifdef _WIN32
    sock_t fd = (sock_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    sock_t fd = (sock_t)socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (fd != kInvalid) {
        SetNoDelay(fd);
    }
    return fd;
}

bool Socket::SetNoDelay(sock_t fd)
{
    // disable Nagle: the stream is a live low-latency pipeline of small
    // messages (tiles, cursor, input); without this, small writes are
    // coalesced for up to ~40ms, which showed up as bursty frame delivery
    // and 300-500ms control-frame stalls under load
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one,
                      sizeof(one)) == 0;
}

bool Socket::SetNonBlocking(sock_t fd, bool nonBlocking)
{
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int nf = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, nf) == 0;
#endif
}

bool Socket::WaitReadable(sock_t fd, int timeoutMs)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET((unsigned)fd, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int r = select((int)fd + 1, &set, nullptr, nullptr, &tv);
    return r > 0;
}

bool Socket::WaitWritable(sock_t fd, int timeoutMs)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET((unsigned)fd, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int r = select((int)fd + 1, nullptr, &set, nullptr, &tv);
    return r > 0;
}

bool Socket::Connect(sock_t fd, const std::string& host, uint16_t port, int timeoutMs)
{
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // resolve hostname
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
            return false;
        }
        std::memcpy(&addr.sin_addr, &((sockaddr_in*)res->ai_addr)->sin_addr, 4);
        freeaddrinfo(res);
    }

    SetNonBlocking(fd, true);
    const int r = connect(fd, (const sockaddr*)&addr, sizeof(addr));
    if (r != 0) {
        const int err = LastErr();
#ifdef _WIN32
        if (err != WSAEWOULDBLOCK) {
            SetNonBlocking(fd, false);
            return false;
        }
#else
        if (err != EINPROGRESS) {
            SetNonBlocking(fd, false);
            return false;
        }
#endif
        // wait for writable
        if (!WaitWritable(fd, timeoutMs)) {
            SetNonBlocking(fd, false);
            return false;
        }
        // check connect error
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len);
        if (soErr != 0) {
            SetNonBlocking(fd, false);
            return false;
        }
    }
    SetNonBlocking(fd, false);
    return true;
}

Socket::sock_t Socket::Listen(uint16_t port)
{
    sock_t fd = CreateTcp();
    if (fd == kInvalid) {
        return kInvalid;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (const sockaddr*)&addr, sizeof(addr)) != 0) {
        Close(fd);
        return kInvalid;
    }
    if (listen(fd, 8) != 0) {
        Close(fd);
        return kInvalid;
    }
    return fd;
}

Socket::sock_t Socket::Accept(sock_t listenFd, std::string& peerIp, uint16_t& peerPort)
{
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const sock_t fd = (sock_t)accept(listenFd, (sockaddr*)&addr, &len);
    if (fd == kInvalid) {
        return kInvalid;
    }
    SetNoDelay(fd); // accepted sockets do not inherit the listener's options
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    peerIp = buf;
    peerPort = ntohs(addr.sin_port);
    return fd;
}

bool Socket::SendAll(sock_t fd, const void* data, size_t len, int timeoutMs)
{
    const char* p = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        if (!WaitWritable(fd, timeoutMs)) {
            return false;
        }
#ifdef _WIN32
        const int n = (int)send(fd, p + sent, (int)(len - sent), 0);
#else
        const ssize_t n = send(fd, p + sent, len - sent, 0);
#endif
        if (n <= 0) {
            return false; // closed or error
        }
        sent += (size_t)n;
    }
    return true;
}

bool Socket::RecvExact(sock_t fd, void* data, size_t len, int timeoutMs)
{
    char* p = (char*)data;
    size_t got = 0;
    while (got < len) {
        if (!WaitReadable(fd, timeoutMs)) {
            return false;
        }
#ifdef _WIN32
        const int n = (int)recv(fd, p + got, (int)(len - got), 0);
#else
        const ssize_t n = recv(fd, p + got, len - got, 0);
#endif
        if (n <= 0) {
            return false; // closed or error
        }
        got += (size_t)n;
    }
    return true;
}

void Socket::Shutdown(sock_t fd)
{
    if (fd == kInvalid) {
        return;
    }
#ifdef _WIN32
    shutdown(fd, SD_BOTH);
#else
    shutdown(fd, SHUT_RDWR);
#endif
}

void Socket::Close(sock_t fd)
{
    if (fd == kInvalid) {
        return;
    }
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

// ---------------------------------------------------------------- UDP

Socket::sock_t Socket::CreateUdp(bool broadcast)
{
    sock_t fd = (sock_t)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == kInvalid) {
        return kInvalid;
    }
    if (broadcast) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&opt, sizeof(opt));
    }
    return fd;
}

bool Socket::UdpBind(sock_t fd, uint16_t port)
{
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifndef _WIN32
    // multiple StarDesk instances on one machine (loopback testing) can all
    // bind the discovery port; macOS/BSD require SO_REUSEPORT for that
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return bind(fd, (const sockaddr*)&addr, sizeof(addr)) == 0;
}

bool Socket::UdpSend(sock_t fd, const void* data, size_t len,
                     const std::string& host, uint16_t port)
{
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return false;
    }
    const int n = (int)sendto(fd, (const char*)data, (int)len, 0,
                              (const sockaddr*)&addr, sizeof(addr));
    return n == (int)len;
}

int Socket::UdpRecv(sock_t fd, void* data, size_t len, int timeoutMs,
                    std::string& fromIp, uint16_t& fromPort)
{
    if (!WaitReadable(fd, timeoutMs)) {
        return -1;
    }
    sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    const int n = (int)recvfrom(fd, (char*)data, (int)len, 0,
                                (sockaddr*)&addr, &addrLen);
    if (n < 0) {
        return -1;
    }
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    fromIp = buf;
    fromPort = ntohs(addr.sin_port);
    return n;
}

// ---------------------------------------------------------------- helpers

std::vector<std::string> Socket::GetLocalIPv4s()
{
    std::vector<std::string> result;
#ifdef _WIN32
    // GetAdaptersAddresses
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &size);
    if (size == 0) {
        return result;
    }
    std::vector<uint8_t> buf(size);
    IP_ADAPTER_ADDRESSES* adapters = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &size) != NO_ERROR) {
        return result;
    }
    for (IP_ADAPTER_ADDRESSES* a = adapters; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr;
             u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET,
                      &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr,
                      ip, sizeof(ip));
            const std::string s(ip);
            if (s != "127.0.0.1") {
                result.push_back(s);
            }
        }
    }
#else
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) {
        return result;
    }
    for (ifaddrs* it = ifa; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &((sockaddr_in*)it->ifa_addr)->sin_addr, ip, sizeof(ip));
        result.push_back(ip);
    }
    freeifaddrs(ifa);
#endif
    if (result.empty()) {
        result.push_back("127.0.0.1");
    }
    return result;
}

std::string Socket::LastError()
{
#ifdef _WIN32
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, WSAGetLastError(), 0, buf, sizeof(buf), nullptr);
    return buf;
#else
    return std::strerror(errno);
#endif
}

} // namespace sdk
