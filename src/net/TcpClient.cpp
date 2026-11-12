#include "TcpClient.h"

namespace sdk {

Socket::sock_t TcpClient::ConnectTo(const std::string& host, uint16_t port, int timeoutMs)
{
    const Socket::sock_t fd = Socket::CreateTcp();
    if (fd == Socket::kInvalid) {
        return Socket::kInvalid;
    }
    if (!Socket::Connect(fd, host, port, timeoutMs)) {
        Socket::Close(fd);
        return Socket::kInvalid;
    }
    return fd;
}

} // namespace sdk
