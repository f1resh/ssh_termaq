#pragma once
// Owns all live connections. Bridges UI <-> Transport:
//   UI input  : write(utf8) -> LineDiscipline (local echo/edit) -> charset encode -> Transport::write
//   output    : Transport bytes -> SessionLog -> charset decode (streaming) -> batched "conn:onData" events
//   prompts   : implements Prompter per connection ("prompts:onPrompt" / respondPrompt / "prompts:onCancel")
//   status    : "conn:onStatus" events, "conn:onEventLog" events, per-connection event log ring buffer (max 2000)
// Events emitted (see ui/src/shared/ipc.ts): conn:onData, conn:onStatus, conn:onEventLog, prompts:onPrompt, prompts:onCancel.
// Output batching: data is coalesced per connection and flushed at most every ~8 ms or when > 64 KiB.
// Thread-safe.
#include "core/Types.h"
#include "transport/Transport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st {

class Store;
class Bridge;
class Transport;

class SessionManager {
public:
    SessionManager(Store& store, Bridge& bridge);
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /** Starts a connection. Throws std::invalid_argument for invalid requests (duplicate connId, empty host...). */
    void open(const ConnOpenRequest& req);
    /** `binary`: data is a "binary string" (each code point < 256 is one byte), bypasses charset encoding. */
    void write(const std::string& connId, const std::string& utf8, bool binary);
    void resize(const std::string& connId, int cols, int rows, int widthPx, int heightPx);
    void close(const std::string& connId);
    void special(const std::string& connId, const std::string& commandId);
    std::vector<SpecialCommandDef> specials(const std::string& connId) const;
    std::vector<EventLogEntry> eventLog(const std::string& connId) const;
    void respondPrompt(const std::string& promptId, const PromptResponse& response);

    /** Live transport for a connection (nullptr if unknown/closed). Used by SFTP. */
    std::shared_ptr<Transport> transport(const std::string& connId) const;
    /** Number of connections that are connecting/connected. */
    int activeCount() const;
    /** Close everything (app shutdown); blocks until transports are torn down (bounded wait). */
    void closeAll();

    /** Builds transports (default: createTransport from transport/TransportFactory.h). Tests inject fakes.
     *  Must be set before the first open(); an empty function restores the default. */
    using TransportFactoryFn = std::function<std::unique_ptr<Transport>(TransportContext ctx)>;
    void setTransportFactory(TransportFactoryFn factory);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Registers conn:* and prompts:* bridge handlers. */
void registerConnHandlers(Bridge& bridge, SessionManager& sessions);

}  // namespace st
