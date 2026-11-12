#include "Protocol.h"

#include <cstring>

namespace sdk {

// ---------------------------------------------------------------- framing

bool SendFrame(Socket::sock_t fd, MsgType type, const void* payload, size_t len,
               int timeoutMs)
{
    uint8_t header[9];
    header[0] = (uint8_t)(kMagic & 0xFF);
    header[1] = (uint8_t)((kMagic >> 8) & 0xFF);
    header[2] = (uint8_t)((kMagic >> 16) & 0xFF);
    header[3] = (uint8_t)((kMagic >> 24) & 0xFF);
    header[4] = (uint8_t)type;
    header[5] = (uint8_t)(len & 0xFF);
    header[6] = (uint8_t)((len >> 8) & 0xFF);
    header[7] = (uint8_t)((len >> 16) & 0xFF);
    header[8] = (uint8_t)((len >> 24) & 0xFF);
    if (!Socket::SendAll(fd, header, sizeof(header), timeoutMs)) {
        return false;
    }
    if (len > 0 && !Socket::SendAll(fd, payload, len, timeoutMs)) {
        return false;
    }
    return true;
}

bool SendFrame(Socket::sock_t fd, MsgType type, const void* payload, size_t len)
{
    return SendFrame(fd, type, payload, len, 5000);
}

bool SendFrame(Socket::sock_t fd, MsgType type, const std::vector<uint8_t>& payload)
{
    return payload.empty() ? SendFrame(fd, type, nullptr, 0)
                           : SendFrame(fd, type, payload.data(), payload.size());
}

// short send timeout for the live stream path: on a congested/collapsed
// link, a long WaitWritable stalls the whole capture pipeline (the send
// mutex is held), which previously froze the stream for up to 5s per
// network hiccup. 50ms lets the pipeline drop the current frame (the kernel
// buffer is full, so nothing was written - the frame is simply skipped) and
// resume at the next 16ms tick: a flaky link costs a lower-but-live frame
// rate instead of multi-second freezes.
bool SendFrameStream(Socket::sock_t fd, MsgType type, const std::vector<uint8_t>& payload)
{
    return SendFrame(fd, type, payload.data(), payload.size(), 50);
}

bool RecvFrame(Socket::sock_t fd, Frame& frame, int timeoutMs)
{
    uint8_t header[9];
    if (!Socket::RecvExact(fd, header, sizeof(header), timeoutMs)) {
        return false;
    }
    const uint32_t magic = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                           ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
    if (magic != kMagic) {
        return false;
    }
    const uint32_t len = (uint32_t)header[5] | ((uint32_t)header[6] << 8) |
                         ((uint32_t)header[7] << 16) | ((uint32_t)header[8] << 24);
    if (len > 64 * 1024 * 1024) {
        return false; // sanity
    }
    frame.type = (MsgType)header[4];
    frame.payload.resize(len);
    if (len > 0 && !Socket::RecvExact(fd, frame.payload.data(), len, timeoutMs)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- payload

void PayloadWriter::PutU8(uint8_t v) { m_buf.push_back(v); }

void PayloadWriter::PutU16(uint16_t v)
{
    m_buf.push_back((uint8_t)(v & 0xFF));
    m_buf.push_back((uint8_t)((v >> 8) & 0xFF));
}

void PayloadWriter::PutU32(uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        m_buf.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
    }
}

void PayloadWriter::PutU64(uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        m_buf.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
    }
}

void PayloadWriter::PutBytes(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    m_buf.insert(m_buf.end(), p, p + len);
}

void PayloadWriter::PutString(const std::string& s)
{
    PutU16((uint16_t)s.size());
    PutBytes(s.data(), s.size());
}

uint8_t PayloadReader::GetU8()
{
    if (m_n < 1) { m_ok = false; return 0; }
    const uint8_t v = m_p[0];
    m_p += 1; m_n -= 1;
    return v;
}

uint16_t PayloadReader::GetU16()
{
    if (m_n < 2) { m_ok = false; return 0; }
    const uint16_t v = (uint16_t)(m_p[0] | ((uint16_t)m_p[1] << 8));
    m_p += 2; m_n -= 2;
    return v;
}

uint32_t PayloadReader::GetU32()
{
    if (m_n < 4) { m_ok = false; return 0; }
    const uint32_t v = (uint32_t)m_p[0] | ((uint32_t)m_p[1] << 8) |
                       ((uint32_t)m_p[2] << 16) | ((uint32_t)m_p[3] << 24);
    m_p += 4; m_n -= 4;
    return v;
}

uint64_t PayloadReader::GetU64()
{
    if (m_n < 8) { m_ok = false; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (uint64_t)m_p[i] << (i * 8);
    }
    m_p += 8; m_n -= 8;
    return v;
}

bool PayloadReader::GetBytes(void* out, size_t len)
{
    if (m_n < len) { m_ok = false; return false; }
    std::memcpy(out, m_p, len);
    m_p += len; m_n -= len;
    return true;
}

bool PayloadReader::GetString(std::string& out)
{
    const uint16_t len = GetU16();
    if (!m_ok || m_n < len) {
        m_ok = false;
        return false;
    }
    out.assign((const char*)m_p, len);
    m_p += len; m_n -= len;
    return true;
}

} // namespace sdk
