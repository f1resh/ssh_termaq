#include "sftp/LocalOpen.h"

#include "core/Uuid.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"
#include "platform/ui/Shell.h"
#include "sftp/RemoteFs.h"

#include <chrono>
#include <mutex>

namespace st::sftpimpl {

namespace fs = std::filesystem;

namespace {

/** Removes temp folders older than a week (best effort, once per process). */
void pruneTemp(const fs::path& root) {
    static std::once_flag once;
    std::call_once(once, [&root] {
        std::error_code ec;
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_directory(ec2) && it->last_write_time(ec2) < cutoff) fs::remove_all(it->path(), ec2);
        }
    });
}

}  // namespace

fs::path chooseTempFolder(const fs::path& systemTemp,
                          const std::function<std::optional<std::string>(const std::string&)>& env, const fs::path& home) {
    if (!systemTemp.empty()) return systemTemp;
    for (const char* name : {"TMPDIR", "TMP", "TEMP"}) {
        const auto value = env(name);
        if (!value || value->empty()) continue;
        const fs::path p = sys::toPath(*value);
        if (p.is_absolute()) return p;
    }
    return home;
}

fs::path tempRoot() {
    std::error_code ec;
    const fs::path systemTemp = fs::temp_directory_path(ec);
    const fs::path base = chooseTempFolder(ec ? fs::path() : systemTemp, sys::getEnv, sys::homeDirectory());
    if (base.empty()) throw SftpError("Cannot find a folder for temporary files (check the TEMP variable)");
    return base / "SSHTermaq";
}

fs::path newTempFolder() {
    const fs::path root = tempRoot();
    pruneTemp(root);
    const fs::path dir = root / newUuid().substr(0, 8);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw SftpError("Cannot create a temporary folder: " + ec.message());
    return dir;
}

void shellOpen(const fs::path& file) {
    sys::markDownloaded(file);
    if (sys::isExecutableType(file)) {
        ui::shell::showInFolder(file);  // show the file instead of running downloaded code
        return;
    }
    ui::shell::openFile(file);
}

void shellEdit(const fs::path& file, const std::string& editor) {
    sys::markDownloaded(file);
    ui::shell::editFile(file, editor);
}

}  // namespace st::sftpimpl
