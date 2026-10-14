#pragma once
// Remote files and folders as a virtual-file drag source (see dnd/DragDrop.h): listed lazily when Explorer asks,
// each file streamed through RemoteFs::openRead while Explorer copies it.
#include "dnd/DragDrop.h"
#include "sftp/RemoteFs.h"

#include <memory>
#include <string>
#include <vector>

namespace st::sftpimpl {

std::shared_ptr<dnd::VirtualSource> makeRemoteDragSource(std::shared_ptr<RemoteFs> fs, std::vector<std::string> paths);

}  // namespace st::sftpimpl
