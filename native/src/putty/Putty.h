#pragma once
// PuTTY interoperability: registry access, session-name escaping, settings <-> SessionConfig mapping and
// SshHostKeys conversion. Mirrors PuTTY 0.83 (settings.c / winstore.c); values missing from a saved session
// take PuTTY's defaults so that an imported session behaves as it did in PuTTY.
#include "core/Types.h"
#include "putty/RegFile.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace st::putty {

inline constexpr const char* kSessionsKey = "Software\\SimonTatham\\PuTTY\\Sessions";
inline constexpr const char* kHostKeysKey = "Software\\SimonTatham\\PuTTY\\SshHostKeys";

/** PuTTY's mungestr() inverse: "%XX" escapes over ANSI bytes -> UTF-8 name. */
std::string unescapeSessionName(const std::string& keyName);
/** mungestr(): UTF-8 name -> registry key name. */
std::string escapeSessionName(const std::string& name);

/** Reads HKCU PuTTY sessions (escaped key name -> values). Missing key => empty. */
std::vector<std::pair<std::string, RegValues>> readRegistrySessions();
/** Reads HKCU SshHostKeys (value name -> data). */
std::vector<std::pair<std::string, std::string>> readRegistryHostKeys();

/** Maps a PuTTY saved session to a SessionConfig (fresh id, name/folder left to the caller). */
SessionConfig puttyToSession(const std::string& name, const RegValues& v, std::vector<std::string>& warnings);
/** Maps a SessionConfig to PuTTY values (for .reg export). */
RegValues sessionToPutty(const SessionConfig& s);

/** "ssh-ed25519@22:host" + "0x..,0x.." -> KnownHost (source 'putty-import'); nullopt + reason when unsupported. */
std::optional<KnownHost> convertHostKey(const std::string& valueName, const std::string& data, std::string& reason);

/** PuTTY LineCodePage display name -> our charset name ('' => UTF-8). */
std::string charsetFromPutty(const std::string& lineCodePage);

}  // namespace st::putty
