#include "FileTransfer.h"

#include "../app/AppConfig.h"
#include "../net/Protocol.h"
#include "../net/TcpClient.h"
#include "../net/TcpServer.h"

#include "dui/Utils/FilePathUtil.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sdk {

namespace {

const uint64_t kChunkSize = 64 * 1024;

bool ReadChunk(FILE* f, uint64_t offset, uint8_t* buf, size_t len)
{
#ifdef _WIN32
    if (_fseeki64(f, (__int64)offset, SEEK_SET) != 0) {
#else
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) {
#endif
        return false;
    }
    return fread(buf, 1, len, f) == len;
}

} // namespace

// ---------------------------------------------------------------- lifecycle

FileTransfer::FileTransfer()
{
}

FileTransfer::~FileTransfer()
{
    Stop();
}

bool FileTransfer::GetToken(uint8_t out[32]) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::memcpy(out, m_token, 32);
    return true;
}

void FileTransfer::Start(uint16_t port, const uint8_t token[32], ItemCb cb)
{
    if (m_running) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::memcpy(m_token, token, 32);
        m_cb = std::move(cb);
    }
    // bind the file port; when it is taken (e.g. a second StarDesk instance
    // on this machine) fall back to the next free port in a small range so
    // both instances can receive files at once
    m_server = std::make_unique<TcpServer>();
    m_port = 0;
    for (int i = 0; i < 10 && m_port == 0; ++i) {
        const uint32_t p = (uint32_t)port + (uint32_t)i;
        if (p > 65535) {
            break;
        }
        if (m_server->Start((uint16_t)p, [this](Socket::sock_t fd, const std::string& ip) {
                OnAccept(fd, ip);
            })) {
            m_port = (uint16_t)p;
        }
    }
    m_running = m_port != 0;
}

void FileTransfer::SetSessionToken(const uint8_t token[32])
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::memcpy(m_token, token, 32);
}

void FileTransfer::SetItemCallback(ItemCb cb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cb = std::move(cb);
}

void FileTransfer::Stop()
{
    if (!m_running && !m_sending) {
        return;
    }
    m_running = false;
    if (m_server) {
        m_server->Stop();
        m_server.reset();
    }
    // wake the blocked recv loops WITHOUT releasing fd numbers (fd-reuse race);
    // the worker threads close their own fds
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Socket::sock_t fd : m_connFds) {
            Socket::Shutdown(fd);
        }
        Socket::Shutdown(m_sendFd);
    }
    if (m_sending) {
        m_sending = false;
    }
    if (m_sendThread.joinable()) {
        // the send thread may still be finishing even when m_sending was
        // already reset (or a worker exception raced the flag) - always join
        m_sendThread.join();
    }
    std::vector<std::thread> conns;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        conns.swap(m_connThreads);
        m_connFds.clear();
        m_sendFd = Socket::kInvalid;
    }
    for (auto& t : conns) {
        if (t.joinable()) {
            t.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.clear();
    }
}

void FileTransfer::OnAccept(Socket::sock_t fd, const std::string& /*peerIp*/)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connFds.push_back(fd);
        m_connThreads.emplace_back(&FileTransfer::RecvLoop, this, fd);
    }
}

void FileTransfer::Notify(uint32_t id)
{
    ItemCb cb;
    Item item;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_items.find(id);
        if (it == m_items.end()) {
            return;
        }
        item = it->second;
        cb = m_cb;
    }
    if (cb) {
        cb(item);
    }
}

// ---------------------------------------------------------------- receive side

void FileTransfer::RecvLoop(Socket::sock_t fd)
{
    // ---- auth ----
    Frame frame;
    if (!RecvFrame(fd, frame, 10000) || frame.type != MsgType::FileAuth ||
        frame.payload.size() != 32) {
        Socket::Close(fd);
        return;
    }
    uint8_t expected[32];
    GetToken(expected);
    if (std::memcmp(expected, frame.payload.data(), 32) != 0) {
        Socket::Close(fd);
        return; // bad token
    }

    uint32_t currentId = 0;
    FILE* outFile = nullptr;
    bool dirEntry = false; // current FileStart was a directory (no file write)
    ui::FilePath outPath;
    uint64_t expectedSize = 0;

    auto failCurrent = [&]() {
        if (outFile != nullptr) {
            std::fclose(outFile);
            outFile = nullptr;
        }
        dirEntry = false;
        if (currentId != 0) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_items.find(currentId);
                if (it != m_items.end()) {
                    it->second.state = ItemState::Failed;
                }
            }
            Notify(currentId);
            currentId = 0;
        }
    };

    while (m_running) {
        if (!RecvFrame(fd, frame, 30000)) {
            failCurrent();
            break;
        }
        PayloadReader r(frame.payload.data(), frame.payload.size());
        switch (frame.type) {
        case MsgType::FileStart: {
            if (currentId != 0) {
                failCurrent(); // no parallel files on one connection
            }
            const uint32_t id = r.GetU32();
            std::string name;
            r.GetString(name);
            const uint64_t size = r.GetU64();
            std::string targetDir;
            if (r.Ok() && r.Remaining() > 0) {
                r.GetString(targetDir); // optional (file browser)
            }
            bool isDir = false;
            if (r.Ok() && r.Remaining() > 0) {
                isDir = r.GetU8() != 0;
            }
            if (!r.Ok() || name.empty() || name.size() > 512) {
                continue;
            }
            // reject path separators in the name (path traversal guard)
            for (char c : name) {
                if (c == '/' || c == '\\') {
                    name = "unnamed";
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                Item item;
                item.id = id;
                item.name = name;
                item.size = size;
                item.incoming = true;
                m_items[id] = item;
            }
            currentId = id;
            expectedSize = size;

            // always accept in phase 1; save into the target dir when the
            // sender provided one (file browser), else the receive directory.
            // Browser paths are relative to the host user's home.
            ui::FilePath dir;
            if (!targetDir.empty()) {
                dir = ui::FilePath(ui::StringConvert::UTF8ToT(targetDir));
                if (dir.IsRelativePath()) {
                    std::string home;
#if defined(_WIN32)
                    if (const char* p = std::getenv("USERPROFILE")) home = p;
#else
                    if (const char* p = std::getenv("HOME")) home = p;
#endif
                    if (!home.empty()) {
                        ui::FilePath homeDir(ui::StringConvert::UTF8ToT(home));
                        homeDir.JoinFilePath(dir);
                        dir = homeDir;
                    }
                }
            }
            else {
                // empty target = the browser's home view (or any sender that
                // did not pick a folder): save under the user's home. Legacy
                // send windows pass their receive dir explicitly.
                std::string home;
#if defined(_WIN32)
                if (const char* p = std::getenv("USERPROFILE")) home = p;
#else
                if (const char* p = std::getenv("HOME")) home = p;
#endif
                dir = ui::FilePath(ui::StringConvert::UTF8ToT(
                    home.empty() ? AppConfig::GetDefaultReceiveDir().ToString() : home));
            }
            if (!dir.IsExistsDirectory()) {
                ui::FilePathUtil::CreateDirectories(dir.ToString());
            }
            
            dirEntry = isDir;
            outPath = dir;
            outPath.JoinFilePath(ui::FilePath(ui::StringConvert::UTF8ToT(name)));
            
            if (!isDir) {
                // regular file: create it for writing
                outFile = fopen(outPath.NativePathA().c_str(), "wb");
                if (outFile == nullptr) {
                    PayloadWriter w;
                    w.PutU32(id);
                    w.PutU8(0);
                    SendFrame(fd, MsgType::FileAccept, w.Result());
                    failCurrent();
                    continue;
                }
            }
            else {
                // directory entry: create the folder itself
                ui::FilePathUtil::CreateDirectories(outPath.ToString());
            }
            
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_items.find(id);
                if (it != m_items.end()) {
                    it->second.state = ItemState::Transferring;
                }
            }
            
            Notify(id);
            
            PayloadWriter w;
            w.PutU32(id);
            w.PutU8(1);
            SendFrame(fd, MsgType::FileAccept, w.Result());
            
            break;
        }
        case MsgType::FileChunk: {
            if (currentId == 0 || outFile == nullptr) {
                continue;
            }
            const uint32_t id = r.GetU32();
            const uint64_t offset = r.GetU64();
            const size_t dataLen = frame.payload.size() - 12;
            if (!r.Ok() || id != currentId) {
                continue;
            }
            if (dataLen > 0) {
#ifdef _WIN32
                const bool seekOk = _fseeki64(outFile, (__int64)offset, SEEK_SET) == 0;
#else
                const bool seekOk = fseeko(outFile, (off_t)offset, SEEK_SET) == 0;
#endif
                if (!seekOk || fwrite(frame.payload.data() + 12, 1, dataLen, outFile) != dataLen) {
                    failCurrent();
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_items.find(id);
                if (it != m_items.end()) {
                    it->second.transferred = offset + dataLen;
                }
            }
            Notify(id);
            break;
        }
        case MsgType::FileEnd: {
            if (currentId == 0) {
                continue;
            }
            const uint32_t id = r.GetU32();
            if (id != currentId) {
                continue;
            }
            if (outFile != nullptr) {
                std::fclose(outFile);
                outFile = nullptr;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_items.find(id);
                if (it != m_items.end()) {
                    it->second.state = ItemState::Done;
                    it->second.transferred = it->second.size;
                }
            }
            Notify(id);
            PayloadWriter w;
            w.PutU32(id);
            SendFrame(fd, MsgType::FileDone, w.Result());
            currentId = 0;
            dirEntry = false;
            break;
        }
        case MsgType::FileAbort: {
            const uint32_t id = r.GetU32();
            if (id == currentId) {
                if (outFile != nullptr) {
                    std::fclose(outFile);
                    outFile = nullptr;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_items.find(id);
                    if (it != m_items.end()) {
                        it->second.state = (it->second.transferred == 0)
                                               ? ItemState::Rejected
                                               : ItemState::Cancelled;
                    }
                }
                Notify(id);
                currentId = 0;
            }
            break;
        }
        default:
            break;
        }
    }

    if (outFile != nullptr) {
        std::fclose(outFile);
    }
    Socket::Close(fd);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connFds.erase(std::remove(m_connFds.begin(), m_connFds.end(), fd),
                        m_connFds.end());
    }
}

// ---------------------------------------------------------------- send side

bool FileTransfer::SendFiles(const std::string& host, uint16_t port,
                             const uint8_t token[32],
                             const std::vector<ui::FilePath>& files,
                             const std::string& targetDir,
                             ItemCb cb, std::string& err)
{
    if (m_sending) {
        err = "busy";
        return false;
    }
    if (files.empty()) {
        err = "empty";
        return false;
    }
    m_sending = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cb = std::move(cb);
        std::memcpy(m_token, token, 32);
        for (const ui::FilePath& f : files) {
            Item item;
            item.id = m_nextId++;
            item.name = ui::StringConvert::TToUTF8(f.GetFileName());
            item.size = f.IsExistsFile() ? (uint64_t)f.GetFileSize() : 0;
            item.state = ItemState::Waiting;
            m_items[item.id] = item;
        }
    }
    m_sendThread = std::thread([this, host, port, files, targetDir]() {
        try {
        const Socket::sock_t fd = TcpClient::ConnectTo(host, port, 5000);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sendFd = fd;
        }
        if (fd == Socket::kInvalid) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& kv : m_items) {
                if (!kv.second.incoming && kv.second.state == ItemState::Waiting) {
                    kv.second.state = ItemState::Failed;
                }
            }
            for (auto& kv : m_items) {
                if (!kv.second.incoming && kv.second.state == ItemState::Failed) {
                    Notify(kv.first);
                }
            }
            m_sending = false;
            return;
        }
        PayloadWriter w;
        w.PutBytes(m_token, 32);
        SendFrame(fd, MsgType::FileAuth, w.Result());
        SendLoop(fd, files, targetDir);
        Socket::Close(fd);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sendFd = Socket::kInvalid;
        }
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[FileTransfer] send thread exception: %s\n", e.what());
            fflush(stderr);
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& kv : m_items) {
                if (!kv.second.incoming) {
                    kv.second.state = ItemState::Failed;
                }
            }
        }
        m_sending = false;
    });
    return true;
}

void FileTransfer::SendLoop(Socket::sock_t fd, const std::vector<ui::FilePath>& files,
                            const std::string& targetDir)
{
    // ids for entries below the top level (no UI item); top-level files and
    // folders keep their item id so progress/state still reaches the UI
    uint32_t subId = 0x40000000;

    auto setItemState = [this](uint32_t id, ItemState st) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_items.find(id);
        if (it != m_items.end()) {
            it->second.state = st;
        }
    };
    auto setProgress = [this](uint32_t id, uint64_t transferred) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_items.find(id);
        if (it != m_items.end()) {
            it->second.transferred = transferred;
        }
    };

    // FileStart + wait FileAccept (accept flag is payload[4], after the u32 id)
    auto sendStart = [&](uint32_t id, const std::string& name, uint64_t size,
                         const std::string& dir, bool isDir) -> bool {
        PayloadWriter w;
        w.PutU32(id);
        w.PutString(name);
        w.PutU64(size);
        w.PutString(dir);
        w.PutU8(isDir ? 1 : 0);
        if (!SendFrame(fd, MsgType::FileStart, w.Result())) {
            return false;
        }
        Frame frame;
        return RecvFrame(fd, frame, 15000) && frame.type == MsgType::FileAccept &&
               frame.payload.size() >= 5 && frame.payload[4] != 0;
    };
    // FileEnd + wait FileDone
    auto sendEnd = [&](uint32_t id) -> bool {
        PayloadWriter w;
        w.PutU32(id);
        if (!SendFrame(fd, MsgType::FileEnd, w.Result())) {
            return false;
        }
        Frame frame;
        return RecvFrame(fd, frame, 15000) && frame.type == MsgType::FileDone;
    };

    // one file: send the payload + chunks
    auto sendFilePayload = [&](uint32_t id, const ui::FilePath& path,
                               const std::string& target, uint64_t size) -> bool {
        FILE* f = fopen(path.NativePathA().c_str(), "rb");
        if (f == nullptr) {
            setItemState(id, ItemState::Failed);
            Notify(id);
            return false;
        }
        const std::string name = ui::StringConvert::TToUTF8(path.GetFileName());
        if (!sendStart(id, name, size, target, false)) {
            std::fclose(f);
            setItemState(id, ItemState::Rejected);
            Notify(id);
            return false;
        }
        uint64_t offset = 0;
        std::vector<uint8_t> buf(kChunkSize);
        bool aborted = false;
        while (offset < size) {
            const size_t len = (size_t)std::min<uint64_t>(kChunkSize, size - offset);
            if (!ReadChunk(f, offset, buf.data(), len)) {
                aborted = true;
                break;
            }
            PayloadWriter w;
            w.PutU32(id);
            w.PutU64(offset);
            w.PutBytes(buf.data(), len);
            if (!SendFrame(fd, MsgType::FileChunk, w.Result())) {
                aborted = true;
                break;
            }
            offset += len;
            setProgress(id, offset);
            Notify(id);
            // peer abort check
            if (m_running && Socket::WaitReadable(fd, 0)) {
                Frame f2;
                if (RecvFrame(fd, f2, 100) && f2.type == MsgType::FileAbort) {
                    aborted = true;
                    break;
                }
            }
        }
        std::fclose(f);
        if (aborted) {
            PayloadWriter w;
            w.PutU32(id);
            SendFrame(fd, MsgType::FileAbort, w.Result());
            setItemState(id, ItemState::Cancelled);
            Notify(id);
            return false;
        }
        if (!sendEnd(id)) {
            setItemState(id, ItemState::Failed);
            Notify(id);
            return false;
        }
        setItemState(id, ItemState::Done);
        Notify(id);
        return true;
    };

    // recursive folder walk: a dir-entry FileStart (isDir) then its contents
    std::function<bool(const ui::FilePath&, const std::string&, uint32_t)> sendFolder =
        [&](const ui::FilePath& dir, const std::string& target, uint32_t itemId) -> bool {
        const std::string name = ui::StringConvert::TToUTF8(dir.GetFileName());
        const uint32_t id = (itemId != 0) ? itemId : ++subId;
        if (!sendStart(id, name, 0, target, true)) {
            setItemState(id, ItemState::Rejected);
            Notify(id);
            return false;
        }
        if (!sendEnd(id)) {
            setItemState(id, ItemState::Failed);
            Notify(id);
            return false;
        }
        const std::string subTarget = target.empty() ? name : target + "/" + name;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir.NativePathA(), ec);
        if (ec) {
            // unreadable folder: skip it but keep the rest of the upload going
            setItemState(id, ItemState::Done);
            Notify(id);
            return true;
        }
        for (const auto& entry : it) {
            if (!m_running && !m_sending) {
                return false;
            }
            std::error_code e2;
            if (entry.is_directory(e2)) {
                const ui::FilePath sub(ui::StringConvert::UTF8ToT(entry.path().string()));
                if (!sendFolder(sub, subTarget, 0)) {
                    return false;
                }
            }
            else if (!e2) {
                const ui::FilePath sub(ui::StringConvert::UTF8ToT(entry.path().string()));
                const uint64_t sz = (uint64_t)entry.file_size(e2);
                if (!sendFilePayload(++subId, sub, subTarget, sz)) {
                    return false;
                }
            }
        }
        return true;
    };

    for (const ui::FilePath& path : files) {
        if (!m_running && !m_sending) {
            break;
        }
        if (path.IsExistsDirectory()) {
            uint32_t itemId = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& kv : m_items) {
                    if (!kv.second.incoming && kv.second.state == ItemState::Waiting &&
                        kv.second.name == ui::StringConvert::TToUTF8(path.GetFileName())) {
                        itemId = kv.first;
                        kv.second.state = ItemState::Transferring;
                        break;
                    }
                }
            }
            Notify(itemId);
            if (!sendFolder(path, targetDir, itemId)) {
                continue;
            }
        }
        else {
            std::error_code e2;
            const uint64_t size = (uint64_t)std::filesystem::file_size(path.NativePathA(), e2);
            if (e2) {
                continue;
            }
            // top-level file: use its item id for progress
            uint32_t itemId = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& kv : m_items) {
                    if (!kv.second.incoming && kv.second.state == ItemState::Waiting &&
                        kv.second.name == ui::StringConvert::TToUTF8(path.GetFileName())) {
                        itemId = kv.first;
                        kv.second.size = size;
                        kv.second.state = ItemState::Transferring;
                        break;
                    }
                }
            }
            if (itemId == 0) {
                continue;
            }
            Notify(itemId);
            sendFilePayload(itemId, path, targetDir, size);
        }
    }
}

void FileTransfer::Cancel(uint32_t id)
{
    // local side: mark cancelled; the peer learns via socket close or abort
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_items.find(id);
    if (it != m_items.end() && (it->second.state == ItemState::Waiting ||
                                it->second.state == ItemState::Transferring)) {
        it->second.state = ItemState::Cancelled;
        Notify(id);
    }
}

} // namespace sdk
