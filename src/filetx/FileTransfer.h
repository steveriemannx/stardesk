#ifndef STARDESK_FILETX_FILE_TRANSFER_H_
#define STARDESK_FILETX_FILE_TRANSFER_H_

#include "dui/dui.h"

#include "../net/Socket.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sdk {

/** File transfer over a dedicated TCP channel (file port = main port + 1).
 *
 *  Both sides listen on the file port; whoever wants to send connects to the
 *  peer and authenticates with the 32-byte session token exchanged during the
 *  main-channel auth. Chunks are 64 KiB; progress is reported through the
 *  ItemCb callback (worker thread - marshal to the UI yourself).
 *
 *  Message flow (file channel):
 *    sender -> FileAuth{token}
 *    sender -> FileStart{id, name, size}
 *    receiver -> FileAccept{id, accept}          (always accepted in phase 1)
 *    sender -> FileChunk{id, offset, data} xN
 *    sender -> FileEnd{id}
 *    receiver -> FileDone{id}                    (file written)
 *    either  -> FileAbort{id}                    (cancel / error)
 */
class FileTransfer {
public:
    enum class ItemState {
        Waiting,     // accepted, not started yet
        Transferring,
        Done,
        Failed,
        Rejected,
        Cancelled,
    };

    struct Item {
        uint32_t id = 0;
        std::string name;      // file name only
        uint64_t size = 0;
        uint64_t transferred = 0;
        ItemState state = ItemState::Waiting;
        bool incoming = false; // true when we receive, false when we send
    };

    /** Progress/state callback, invoked on worker threads. */
    typedef std::function<void(const Item&)> ItemCb;

    FileTransfer();
    ~FileTransfer();
    FileTransfer(const FileTransfer&) = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    /** Listen on the file port (falls back to port+1..port+9 when taken) and
     *  accept incoming transfers; BoundPort() reports what was bound. */
    void Start(uint16_t port, const uint8_t token[32], ItemCb cb);
    void Stop();

    /** The file port actually bound (0 when Start() found no free port). */
    uint16_t BoundPort() const { return m_port; }

    /** Session token for validating incoming senders; refreshed after each
     *  successful auth on the main channel. */
    void SetSessionToken(const uint8_t token[32]);

    /** Replace the global progress callback (windows register it to render
     *  the transfer list). Thread-safe. */
    void SetItemCallback(ItemCb cb);

    /** Send files to the peer; returns false (with err) when another send is
     *  already in progress. Progress arrives through `cb`. targetDir (empty =
     *  receiver default) is where the peer saves the files (file browser). */
    bool SendFiles(const std::string& host, uint16_t port, const uint8_t token[32],
                   const std::vector<ui::FilePath>& files,
                   const std::string& targetDir, ItemCb cb, std::string& err);

    /** Abort a transfer (send or receive) by id. */
    void Cancel(uint32_t id);

    bool IsSending() const { return m_sending; }

private:
    void OnAccept(Socket::sock_t fd, const std::string& peerIp);
    void RecvLoop(Socket::sock_t fd);
    void SendLoop(Socket::sock_t fd, const std::vector<ui::FilePath>& files,
                  const std::string& targetDir);
    void Notify(uint32_t id);
    bool GetToken(uint8_t out[32]) const;

    std::unique_ptr<class TcpServer> m_server;
    std::vector<std::thread> m_connThreads;
    std::vector<Socket::sock_t> m_connFds; // for wake-up on Stop()
    Socket::sock_t m_sendFd = Socket::kInvalid;
    mutable std::mutex m_mutex;
    uint8_t m_token[32] = {0};
    uint16_t m_port = 0; // bound file port (0 = not running)
    ItemCb m_cb;
    std::map<uint32_t, Item> m_items;
    uint32_t m_nextId = 1;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_sending{false};
    std::thread m_sendThread;
};

} // namespace sdk

#endif // STARDESK_FILETX_FILE_TRANSFER_H_
