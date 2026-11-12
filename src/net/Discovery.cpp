#include "Discovery.h"

#include "../app/AppConfig.h"
#include "../net/Protocol.h"

#include <chrono>
#include <cstring>

namespace sdk {

namespace {

const char kBeaconMagic[4] = { 'S', 'D', 'K', '1' };
const uint8_t kBeaconType = 'B';
const int kExpirySec = 5; // beacons every 2s; drop after 5s of silence

} // namespace

Discovery::Discovery() = default;

Discovery::~Discovery()
{
    Stop();
}

void Discovery::Start(const std::string& deviceName, uint16_t tcpPort, DevicesCb cb)
{
    if (m_running) {
        return;
    }
    m_deviceName = deviceName;
    m_tcpPort = tcpPort;
    m_cb = std::move(cb);
    m_running = true;

    m_beaconFd = Socket::CreateUdp(true /* broadcast */);
    m_listenFd = Socket::CreateUdp(false);
    if (m_listenFd != Socket::kInvalid) {
        // multiple StarDesk instances on one machine can share the discovery port
        Socket::UdpBind(m_listenFd, kDiscoveryPort);
    }
    m_beaconThread = std::thread(&Discovery::BeaconLoop, this);
    m_listenThread = std::thread(&Discovery::ListenLoop, this);
}

void Discovery::Stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    // wake the blocked recvfrom without releasing fd numbers
    Socket::Shutdown(m_listenFd);
    Socket::Shutdown(m_beaconFd);
    if (m_beaconThread.joinable()) {
        m_beaconThread.join();
    }
    if (m_listenThread.joinable()) {
        m_listenThread.join();
    }
    Socket::Close(m_beaconFd);
    Socket::Close(m_listenFd);
    m_beaconFd = Socket::kInvalid;
    m_listenFd = Socket::kInvalid;
}

const std::vector<Discovery::Device>& Discovery::Devices() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_devices;
}

void Discovery::BeaconLoop()
{
    if (m_beaconFd == Socket::kInvalid) {
        m_running = false;
        return;
    }
    // payload: magic + type + u16 nameLen + name + u16 port
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), kBeaconMagic, kBeaconMagic + 4);
    payload.push_back(kBeaconType);
    const uint16_t nameLen = (uint16_t)m_deviceName.size();
    payload.push_back((uint8_t)(nameLen & 0xFF));
    payload.push_back((uint8_t)(nameLen >> 8));
    payload.insert(payload.end(), m_deviceName.begin(), m_deviceName.end());
    payload.push_back((uint8_t)(m_tcpPort & 0xFF));
    payload.push_back((uint8_t)(m_tcpPort >> 8));

    // broadcast + loopback (for same-machine testing)
    while (m_running) {
        Socket::UdpSend(m_beaconFd, payload.data(), payload.size(),
                        "255.255.255.255", kDiscoveryPort);
        Socket::UdpSend(m_beaconFd, payload.data(), payload.size(),
                        "127.0.0.1", kDiscoveryPort);
        for (int i = 0; i < 20 && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Discovery::ListenLoop()
{
    if (m_listenFd == Socket::kInvalid) {
        m_running = false;
        return;
    }
    uint8_t buf[512];
    auto lastUpdate = std::chrono::steady_clock::now();
    while (m_running) {
        std::string fromIp;
        uint16_t fromPort = 0;
        const int n = Socket::UdpRecv(m_listenFd, buf, sizeof(buf), 200,
                                      fromIp, fromPort);
        if (n >= 7 && std::memcmp(buf, kBeaconMagic, 4) == 0 && buf[4] == kBeaconType) {
            const uint16_t nameLen = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
            if (5 + nameLen + 2 <= (size_t)n) {
                Device dev;
                dev.name.assign((const char*)buf + 7, nameLen);
                dev.ip = fromIp;
                dev.port = (uint16_t)buf[7 + nameLen] | ((uint16_t)buf[8 + nameLen] << 8);
                if (dev.port > 0 && !dev.name.empty()) {
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        bool found = false;
                        for (Device& d : m_devices) {
                            if (d.ip == dev.ip && d.port == dev.port) {
                                d.lastSeen = std::chrono::steady_clock::now();
                                if (d.name != dev.name) {
                                    d.name = dev.name;
                                    changed = true;
                                }
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            dev.lastSeen = std::chrono::steady_clock::now();
                            m_devices.push_back(dev);
                            changed = true;
                        }
                    }
                    if (changed && m_cb) {
                        m_cb(Devices());
                    }
                }
            }
        }
        // periodic expiry sweep
        const auto now = std::chrono::steady_clock::now();
        if (now - lastUpdate > std::chrono::seconds(3)) {
            lastUpdate = now;
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const size_t before = m_devices.size();
                m_devices.erase(
                    std::remove_if(m_devices.begin(), m_devices.end(),
                                   [now](const Device& d) {
                                       return now - d.lastSeen >
                                              std::chrono::seconds(kExpirySec);
                                   }),
                    m_devices.end());
                changed = m_devices.size() != before;
            }
            if (changed && m_cb) {
                m_cb(Devices());
            }
        }
    }
}

} // namespace sdk
