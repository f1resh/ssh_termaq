#pragma once
// Trusted SSH host keys, persisted as known_hosts.json: { "hosts": [KnownHost...] }.
// Thread-safe.
// Hosts are compared case-insensitively (surrounding whitespace and IPv6 brackets ignored). Key types are compared
// after mapping RSA signature algorithm names (rsa-sha2-256/512) to the key type "ssh-rsa".
#include "core/Types.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace sc {

class KnownHosts {
public:
    explicit KnownHosts(std::filesystem::path file);
    /** Loads known_hosts.json (missing => empty; corrupt => backed up and empty; invalid entries skipped). */
    void load();

    std::vector<KnownHost> list() const;
    /** All keys stored for host:port (host compared case-insensitively). */
    std::vector<KnownHost> find(const std::string& host, int port) const;
    /**
     * Match  : a stored key for host:port equals keyBlob.
     * Mismatch: host:port has stored keys of the same keyType but none equal (possible MITM).
     * Unknown : no stored key of this type for host:port.
     * (A different key *type* than stored ones counts as Unknown, like PuTTY.)
     * keyBlob may be either the raw SSH wire-format blob or its base64 encoding (as stored in KnownHost::key).
     * An empty keyType is derived from the blob.
     */
    HostKeyCheck check(const std::string& host, int port, const std::string& keyType, const std::string& keyBlob) const;
    /** Adds (or replaces an entry with the same host/port/keyType). Fills id/fingerprint/addedAt if empty. */
    KnownHost add(KnownHost entry);
    /** Bulk add; entries identical to existing ones are skipped. Returns number added/replaced. */
    int addMany(const std::vector<KnownHost>& entries);
    void remove(const std::vector<std::string>& ids);

    /** Fired after any change (outside the lock, on the thread that made the change). */
    std::function<void()> onChanged;

private:
    void saveLocked() const;
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::vector<KnownHost> hosts_;
};

}  // namespace sc
