#pragma once
// Import / export of sessions and host keys:
//   PuTTY registry, PuTTY .reg files, ~/.ssh/config, our JSON backup, OpenSSH known_hosts.
// Candidates may reference each other as jump hosts with jumpSessionId = "import:<candidate key>"; applyImport()
// rewrites those references to the ids of the imported sessions.
#include "core/Types.h"

#include <string>
#include <vector>

namespace st {
class Store;
}

namespace st::importers {

ImportPreview previewPuttyRegistry(const Store& store);
/** Throws std::runtime_error when the file cannot be read or is not a .reg export. */
ImportPreview previewPuttyRegFile(const std::string& path, const Store& store);
ImportPreview previewSshConfig(const std::string& path, const Store& store);
ImportPreview previewJsonBackup(const std::string& path, const Store& store);

/** Parses OpenSSH known_hosts text; hashed / @cert-authority / @revoked / malformed lines are counted in `skipped`. */
std::vector<KnownHost> parseOpenSshKnownHosts(const std::string& content, int& skipped);

ImportResult applyImport(Store& store, const ImportRequest& req);

/** UTF-16LE .reg bytes for the given sessions. */
std::string exportPuttyReg(const std::vector<SessionConfig>& sessions);
/** Our JSON backup format (pretty printed). */
std::string exportJsonBackup(const Json& sessions, const std::vector<std::string>& folders);

}  // namespace st::importers
