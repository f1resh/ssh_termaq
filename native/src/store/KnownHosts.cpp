#include "store/KnownHosts.h"

#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Uuid.h"
#include "store/JsonFile.h"

#include <stdexcept>

namespace sc {

namespace {

std::string normHost(const std::string& host) {
    std::string h = trim(host);
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    return toLower(h);
}

/** Host key *type* for an algorithm name (RSA signature algorithms share the "ssh-rsa" key type). */
std::string normKeyType(const std::string& type) {
    if (type == "rsa-sha2-256" || type == "rsa-sha2-512") return "ssh-rsa";
    return type;
}

/** Raw wire blob from either a raw blob or its base64 form (raw blobs start with a NUL byte => never valid base64). */
std::string rawBlob(const std::string& blobOrBase64) {
    std::string decoded;
    if (!blobOrBase64.empty() && crypto::base64Decode(blobOrBase64, decoded) && !decoded.empty()) return decoded;
    return blobOrBase64;
}

std::string storedRaw(const KnownHost& h) {
    std::string raw;
    if (crypto::base64Decode(h.key, raw)) return raw;
    return {};
}

bool sameSlot(const KnownHost& a, const KnownHost& b) {
    return a.port == b.port && normHost(a.host) == normHost(b.host) && normKeyType(a.keyType) == normKeyType(b.keyType);
}

/** Completes an entry (key type, fingerprint, id, timestamp, canonical base64). Throws on unusable input. */
KnownHost complete(KnownHost e) {
    e.host = trim(e.host);
    if (e.host.empty()) throw std::invalid_argument("Known host entry without host name");
    if (e.port <= 0 || e.port > 65535) throw std::invalid_argument("Known host entry with invalid port");
    std::string raw = rawBlob(e.key);
    if (raw.empty()) throw std::invalid_argument("Known host entry without key");
    e.key = crypto::base64Encode(raw);
    std::string blobType = crypto::keyTypeOfBlob(raw);
    if (e.keyType.empty()) e.keyType = blobType;
    e.keyType = normKeyType(e.keyType);
    if (e.keyType.empty()) throw std::invalid_argument("Known host entry without key type");
    if (e.fingerprint.empty()) e.fingerprint = crypto::fingerprintSha256(raw);
    if (e.id.empty()) e.id = newUuid();
    if (e.addedAt == 0) e.addedAt = nowMs();
    if (e.source.empty()) e.source = "user";
    return e;
}

}  // namespace

KnownHosts::KnownHosts(std::filesystem::path file) : file_(std::move(file)) {}

void KnownHosts::load() {
    Json root;
    std::vector<KnownHost> hosts;
    if (storeio::loadJson(file_, root) == storeio::LoadStatus::Ok) {
        const Json* arr = nullptr;
        if (root.is_object() && root.contains("hosts") && root["hosts"].is_array())
            arr = &root["hosts"];
        else if (root.is_array())
            arr = &root;
        else
            storeio::backupCorrupt(file_, "unexpected document shape");
        if (arr) {
            size_t skipped = 0;
            for (const auto& item : *arr) {
                try {
                    hosts.push_back(complete(item.get<KnownHost>()));
                } catch (const std::exception&) {
                    ++skipped;
                }
            }
            if (skipped) applog::warn("KnownHosts: skipped " + std::to_string(skipped) + " invalid entries");
        }
    }
    std::lock_guard lock(mutex_);
    hosts_ = std::move(hosts);
}

std::vector<KnownHost> KnownHosts::list() const {
    std::lock_guard lock(mutex_);
    return hosts_;
}

std::vector<KnownHost> KnownHosts::find(const std::string& host, int port) const {
    const std::string h = normHost(host);
    std::vector<KnownHost> out;
    std::lock_guard lock(mutex_);
    for (const auto& e : hosts_)
        if (e.port == port && normHost(e.host) == h) out.push_back(e);
    return out;
}

HostKeyCheck KnownHosts::check(const std::string& host, int port, const std::string& keyType, const std::string& keyBlob) const {
    const std::string raw = rawBlob(keyBlob);
    std::string type = normKeyType(keyType.empty() ? crypto::keyTypeOfBlob(raw) : keyType);
    const std::string h = normHost(host);
    bool sameType = false;
    std::lock_guard lock(mutex_);
    for (const auto& e : hosts_) {
        if (e.port != port || normHost(e.host) != h) continue;
        if (!raw.empty() && storedRaw(e) == raw) return HostKeyCheck::Match;
        if (normKeyType(e.keyType) == type) sameType = true;
    }
    return sameType ? HostKeyCheck::Mismatch : HostKeyCheck::Unknown;
}

KnownHost KnownHosts::add(KnownHost entry) {
    KnownHost e = complete(std::move(entry));
    {
        std::lock_guard lock(mutex_);
        auto previous = hosts_;
        std::erase_if(hosts_, [&](const KnownHost& x) { return sameSlot(x, e) || x.id == e.id; });
        hosts_.push_back(e);
        try {
            saveLocked();
        } catch (...) {
            hosts_ = std::move(previous);
            throw;
        }
    }
    if (onChanged) onChanged();
    return e;
}

int KnownHosts::addMany(const std::vector<KnownHost>& entries) {
    int changed = 0;
    {
        std::lock_guard lock(mutex_);
        auto previous = hosts_;
        for (const auto& in : entries) {
            KnownHost e;
            try {
                e = complete(in);
            } catch (const std::exception&) {
                continue;  // unusable entry (no host/key)
            }
            bool identical = false;
            for (const auto& x : hosts_)
                if (sameSlot(x, e) && storedRaw(x) == storedRaw(e)) identical = true;
            if (identical) continue;
            std::erase_if(hosts_, [&](const KnownHost& x) { return sameSlot(x, e) || x.id == e.id; });
            hosts_.push_back(e);
            ++changed;
        }
        if (changed) {
            try {
                saveLocked();
            } catch (...) {
                hosts_ = std::move(previous);
                throw;
            }
        }
    }
    if (changed && onChanged) onChanged();
    return changed;
}

void KnownHosts::remove(const std::vector<std::string>& ids) {
    {
        std::lock_guard lock(mutex_);
        auto previous = hosts_;
        size_t n = std::erase_if(hosts_, [&](const KnownHost& x) {
            for (const auto& id : ids)
                if (x.id == id) return true;
            return false;
        });
        if (n == 0) return;
        try {
            saveLocked();
        } catch (...) {
            hosts_ = std::move(previous);
            throw;
        }
    }
    if (onChanged) onChanged();
}

void KnownHosts::saveLocked() const {
    storeio::saveJson(file_, Json{{"hosts", hosts_}});
}

}  // namespace sc
