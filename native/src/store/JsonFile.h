#pragma once
// Internal helpers shared by Store / KnownHosts / Credentials (not part of the public store API).
// Reading tolerates a UTF-8 BOM and // comments (hand-edited files); unparsable files are moved aside as
// "<name>.corrupt-<YYYYMMDD-HHMMSS>" so the next save starts fresh without destroying the user's data.
#include "core/Json.h"

#include <filesystem>
#include <string>

namespace sc::storeio {

enum class LoadStatus { Missing, Ok, Corrupt };

/** Loads and parses `path`. On a parse error the file is backed up (see above) and Corrupt is returned. */
LoadStatus loadJson(const std::filesystem::path& path, Json& out);

/** Moves a corrupt file aside; returns the backup path ('' if the move failed). Logs a warning. */
std::filesystem::path backupCorrupt(const std::filesystem::path& path, const std::string& reason);

/** Serializes with 2-space indentation (invalid UTF-8 replaced) and writes atomically. Throws on failure. */
void saveJson(const std::filesystem::path& path, const Json& value);

}  // namespace sc::storeio
