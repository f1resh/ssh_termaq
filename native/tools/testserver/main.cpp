// Standalone test SSH server for manual / end-to-end testing.
// Usage: sc_testserver [--port N] [--user U] [--password P] [--sftp-root DIR] [--authorized-keys FILE]
//                      [--banner TEXT] [--host-key-type ed25519|rsa|ecdsa]
//                      [--sftp on|off] [--exec-shell git|PATH]
//   --sftp off           refuse the SFTP subsystem (clients fall back to SCP)
//   --exec-shell git     run exec requests through Git for Windows' sh.exe in the SFTP root (real ls / scp)
// Prints "LISTENING <port> <host key fingerprint>" and runs until stdin is closed or Ctrl+C.
#include "TestServers.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    sc::test::TestSshServerOptions o;
    o.port = 2222;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--port") o.port = std::stoi(v);
        else if (k == "--user") o.username = v;
        else if (k == "--password") o.password = v;
        else if (k == "--sftp-root") o.sftpRoot = v;
        else if (k == "--banner") o.banner = v;
        else if (k == "--host-key-type") o.hostKeyType = v;
        else if (k == "--sftp") o.enableSftp = v != "off";
        else if (k == "--exec-shell") {
            o.execShell = v == "git" ? sc::test::gitShell() : std::filesystem::path(v);
            if (o.execShell.empty()) {
                std::cerr << "Git for Windows' sh.exe was not found\n";
                return 2;
            }
        }
        else if (k == "--authorized-keys") {
            std::ifstream f(v);
            for (std::string line; std::getline(f, line);)
                if (!line.empty() && line[0] != '#') o.authorizedKeys.push_back(line);
        } else {
            std::cerr << "unknown option " << k << "\n";
            return 2;
        }
    }
    try {
        sc::test::TestSshServer server(o);
        std::cout << "LISTENING " << server.port() << " " << server.hostKeyFingerprint() << std::endl;
        // Block until stdin closes (or the process is killed).
        for (std::string line; std::getline(std::cin, line);) {
            if (line == "events")
                for (const auto& e : server.events()) std::cout << e << "\n";
            std::cout.flush();
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
