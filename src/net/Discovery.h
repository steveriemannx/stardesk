#ifndef STARDESK_NET_DISCOVERY_H_
#define STARDESK_NET_DISCOVERY_H_

#include "Socket.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sdk {

/** LAN device discovery over UDP: every host broadcasts a small beacon every
 *  2 seconds on kDiscoveryPort; clients listen on the same port and collect
 *  the peers (30s expiry). Useful on a shared hotspot / LAN.
 *
 *  Beacon payload: "SDK1B" + u16 nameLen + name + u16 port.
 */
class Discovery {
public:
    struct Device {
        std::string name;
        std::string ip;
        uint16_t port = 0;
        std::chrono::steady_clock::time_point lastSeen; // internal
    };

    /** Called on the listener thread when the device list changed. */
    typedef std::function<void(const std::vector<Device>&)> DevicesCb;

    Discovery();
    ~Discovery();
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    /** Start beaconing (host role) and listening (client role). */
    void Start(const std::string& deviceName, uint16_t tcpPort, DevicesCb cb);
    void Stop();

    const std::vector<Device>& Devices() const;

private:
    void BeaconLoop();
    void ListenLoop();

    std::thread m_beaconThread;
    std::thread m_listenThread;
    std::atomic<bool> m_running{false};
    std::string m_deviceName;
    uint16_t m_tcpPort = 0;
    DevicesCb m_cb;

    Socket::sock_t m_beaconFd = Socket::kInvalid;
    Socket::sock_t m_listenFd = Socket::kInvalid;

    mutable std::mutex m_mutex;
    std::vector<Device> m_devices;
};

} // namespace sdk

#endif // STARDESK_NET_DISCOVERY_H_
