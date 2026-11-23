// Checkpoint GitHub edition. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GITHUBSYNC_CORE_HPP
#define GITHUBSYNC_CORE_HPP
#include <cstddef>
#include <cstdint>
#include <string>

namespace GitHubSync {
    // Our store-only ZIPs have no archive comment. Require the EOCD and a
    // complete central directory, rather than accepting a truncated prefix.
    inline bool validZipFooter(const unsigned char* b, size_t size, unsigned& entries)
    {
        auto le16 = [&](size_t i) { return unsigned(b[i]) | (unsigned(b[i + 1]) << 8); };
        auto le32 = [&](size_t i) { return unsigned(b[i]) | (unsigned(b[i + 1]) << 8) | (unsigned(b[i + 2]) << 16) | (unsigned(b[i + 3]) << 24); };
        if (size < 22 || le32(0) != 0x06054b50 || le16(4) || le16(6) || le16(8) != le16(10) || le16(20)) return false;
        entries = le16(10);
        return entries > 0 && entries <= 4096 && uint64_t(le32(12)) + le32(16) == size - 22;
    }
    inline bool validRepository(const std::string& s)
    {
        const auto slash = s.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 == s.size() || s.size() > 140) return false;
        for (size_t i = 0; i < s.size(); ++i) {
            const unsigned char c = s[i];
            if (i == slash) continue;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
        }
        const auto name = s.substr(slash + 1);
        return name != "." && name != "..";
    }
    inline bool validBranch(const std::string& s)
    {
        if (s.empty() || s.size() > 200 || s.front() == '/' || s.back() == '/' || s.back() == '.' ||
            s.find("..") != std::string::npos || s.find("@{") != std::string::npos || s.find("//") != std::string::npos || s == "@") return false;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '/') {
                const auto part = s.substr(start, i - start);
                if (part.empty() || part.front() == '.' || part.ends_with(".lock")) return false;
                start = i + 1;
            }
            else if ((unsigned char)s[i] <= 32 || (unsigned char)s[i] >= 127 || std::string("~^:?*[\\").find(s[i]) != std::string::npos) return false;
        }
        return true;
    }
    inline bool safeArchiveName(const std::string& name)
    {
        if (name.size() <= 4 || name.size() > 180 || !name.ends_with(".zip") || name.front() == '.' || name.back() == ' ') return false;
        for (unsigned char c : name) if (c < 32 || c == 127 || std::string("/\\:*?\"<>|").find(c) != std::string::npos) return false;
        const auto base = name.substr(0, name.size() - 4);
        return !base.empty() && base.back() != '.' && base.back() != ' ';
    }
    inline bool safeEntryPath(const std::string& path)
    {
        if (path.empty() || path.size() > 700 || path.front() == '/') return false;
        size_t start = 0, depth = 0;
        for (size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path[i] == '/') {
                if (i == path.size() && start == i) break; // directory's trailing slash
                const auto part = path.substr(start, i - start);
                if (++depth > 32 || part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
                start = i + 1;
            }
            else {
                const auto c = (unsigned char)path[i];
                if (c < 32 || c == 127 || std::string("\\:*?\"<>|").find(c) != std::string::npos) return false;
            }
        }
        return true;
    }
    // Encode complete input chunks; callers use a multiple of three except at EOF.
    inline std::string base64(const unsigned char* data, size_t size)
    {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((size + 2) / 3) * 4);
        for (size_t i = 0; i < size; i += 3) {
            const unsigned a = data[i], b = i + 1 < size ? data[i + 1] : 0, c = i + 2 < size ? data[i + 2] : 0;
            out += alphabet[a >> 2]; out += alphabet[((a & 3) << 4) | (b >> 4)];
            out += i + 1 < size ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
            out += i + 2 < size ? alphabet[c & 63] : '=';
        }
        return out;
    }
}
#endif
