// Checkpoint GitHub edition: private-repository save backups.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GITHUBSYNC_HPP
#define GITHUBSYNC_HPP

#include "githubsync_core.hpp"
#include <string>
#include <vector>

namespace GitHubSync {
    struct Config {
        std::string repository;
        std::string branch = "main";
        bool afterBackup = false;
    };
    struct RemoteBackup {
        std::string name;
        std::string sha;
    };
    Config loadConfig();
    bool saveConfig(const Config& config);
    bool tokenPresent();
    // Blocking operations, called exclusively by TransferJob's worker.
    // Errors are user-facing, never include credentials or response bodies.
    bool upload(const Config&, const std::string& titleId, const std::string& kind,
        const std::u16string& backupPath, std::string& error);
    bool list(const Config&, const std::string& titleId, const std::string& kind,
        std::vector<RemoteBackup>& backups, std::string& error);
    bool download(const Config&, const std::string& titleId, const std::string& kind,
        const RemoteBackup&, const std::u16string& root, std::string& error);
}
#endif
