#pragma once
// Minimal SFTP v3 server protocol engine used by TestSshServer's "sftp" subsystem (internal to tests/support).
// Transport-agnostic: feed() the raw channel bytes, write the produced bytes back to the channel.
// All paths are virtual ("/" == root) and confined to the root directory: ".." never climbs above "/",
// drive letters / alternate streams (':') are rejected, symlink creation is not supported.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace st::test {

class SftpServer {
public:
    explicit SftpServer(std::filesystem::path root);
    ~SftpServer();
    SftpServer(const SftpServer&) = delete;
    SftpServer& operator=(const SftpServer&) = delete;

    /** Consumes channel data; appends complete response packets to `out`. Returns false on a fatal protocol error
     *  (malformed framing / oversized packet) after which the channel should be closed. */
    bool feed(std::string_view data, std::string& out);

    /** Number of currently open file / directory handles (for leak assertions). */
    size_t openHandles() const;

private:
    struct Handle;

    void process(std::string_view packet, std::string& out);
    void sendStatus(std::string& out, uint32_t id, uint32_t code, std::string_view message = {});

    std::filesystem::path root_;
    std::string inbuf_;
    bool initialised_ = false;
    uint64_t nextHandle_ = 1;
    std::map<std::string, std::unique_ptr<Handle>> handles_;
};

}  // namespace st::test
