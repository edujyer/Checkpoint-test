// Checkpoint GitHub edition. SPDX-License-Identifier: GPL-3.0-or-later
#include "githubsync.hpp"
#include "i18n.hpp"
#include "json.hpp"
#include "transferprotocol.hpp"
#include "transferstatus.hpp"
#include "util.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace {
    using Json = nlohmann::json;
    constexpr size_t MAX_ARCHIVE = 32 * 1024 * 1024;
    constexpr size_t MAX_JSON = 2 * 1024 * 1024;
    constexpr const char* CONFIG = "/3ds/Checkpoint/github.json";
    constexpr const char* TOKEN = "/3ds/Checkpoint/github-token.txt";
    constexpr const char* CA = "romfs:/github-ca.pem";
    // These temporary files contain saves, never the token. One TransferJob at a time.
    constexpr const char* ZIP = "/3ds/Checkpoint/github-transfer.zip";
    constexpr const char* BODY = "/3ds/Checkpoint/github-transfer.json.tmp";
    struct FileCloser { void operator()(FILE* f) const { if (f) fclose(f); } };
    using File = std::unique_ptr<FILE, FileCloser>;
    struct TempGuard { ~TempGuard() { std::remove(ZIP); std::remove(BODY); } };

    bool fail(std::string& error, const char* key) { error = i18n::t(key); return false; }
    std::string readToken()
    {
        File f(fopen(TOKEN, "rb"));
        if (!f) return {};
        char buf[257];
        const size_t n = fread(buf, 1, sizeof(buf), f.get());
        if (!n || n == sizeof(buf) || ferror(f.get())) return {};
        std::string s(buf, n);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        if (s.size() < 20) return {};
        for (unsigned char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return {};
        return s;
    }
    std::string encode(const std::string& s)
    {
        // Bytewise RFC 3986 encoding, including '/' in query parameters.
        static const char hex[] = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += c;
            else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        }
        return out;
    }
    std::string contentsUrl(const GitHubSync::Config& cfg, const std::string& id, const std::string& kind, const std::string& name = "")
    {
        return "https://api.github.com/repos/" + cfg.repository + "/contents/checkpoint/3ds/" + encode(id) + "/" + encode(kind) +
            (name.empty() ? "" : "/" + encode(name));
    }
    struct Response {
        std::string body;
        FILE* file = nullptr;
        size_t received = 0;
        size_t limit = MAX_JSON;
        bool failed = false;
    };
    size_t receive(char* data, size_t size, size_t count, void* context)
    {
        auto& r = *static_cast<Response*>(context);
        if (size && count > (r.limit - r.received) / size) { r.failed = true; return 0; }
        const size_t n = size * count;
        try {
            if (r.file) { if (fwrite(data, 1, n, r.file) != n) { r.failed = true; return 0; } }
            else r.body.append(data, n);
            r.received += n;
            return n;
        }
        catch (...) { r.failed = true; return 0; }
    }
    int progress(void*, curl_off_t total, curl_off_t done, curl_off_t upTotal, curl_off_t upDone)
    {
        if (TransferStatus::cancelRequested()) return 1;
        if (upTotal > 0) TransferStatus::setBytes(upDone, upTotal);
        else if (total > 0) TransferStatus::setBytes(done, total);
        return 0;
    }
    // No redirects: a token can only reach api.github.com. Certificate and hostname
    // verification are mandatory. Never reuse the script engine's insecure curl helper.
    bool request(const GitHubSync::Config& cfg, const std::string& method, const std::string& url,
        Response& response, long& status, std::string& error, const char* bodyFile = nullptr, bool raw = false, bool object = false)
    {
        if (!GitHubSync::validRepository(cfg.repository) || !GitHubSync::validBranch(cfg.branch)) return fail(error, "github.config_error");
        const std::string token = readToken();
        if (token.empty()) return fail(error, "github.token_help");
        File certificate(fopen(CA, "rb"));
        if (!certificate) return fail(error, "github.ca_missing");
        certificate.reset();
        static std::once_flag init;
        static CURLcode initCode = CURLE_FAILED_INIT;
        std::call_once(init, [] { initCode = curl_global_init(CURL_GLOBAL_DEFAULT); });
        if (initCode != CURLE_OK) return fail(error, "github.network_error");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) return fail(error, "github.network_error");
        struct curl_slist* headers = nullptr;
        const std::array<std::string, 4> headerLines = {"Authorization: Bearer " + token,
            raw ? "Accept: application/vnd.github.raw+json" : object ? "Accept: application/vnd.github.object+json" : "Accept: application/vnd.github+json",
            "X-GitHub-Api-Version: 2022-11-28", "Content-Type: application/json"};
        for (const auto& line : headerLines) {
            auto* next = curl_slist_append(headers, line.c_str());
            if (!next) { curl_slist_free_all(headers); return fail(error, "github.memory_error"); }
            headers = next;
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerGuard(headers, curl_slist_free_all);
        File body;
        if (bodyFile) {
            body.reset(fopen(bodyFile, "rb"));
            if (!body || fseek(body.get(), 0, SEEK_END) != 0) return fail(error, "github.sd_error");
            const long length = ftell(body.get());
            if (length < 0) return fail(error, "github.sd_error");
            rewind(body.get());
            curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_READDATA, body.get());
            curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE, (curl_off_t)length);
        }
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Checkpoint-GitHub-edition");
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, CA);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 100L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
        const auto code = curl_easy_perform(curl.get());
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        if (code != CURLE_OK) {
            if (TransferStatus::cancelRequested()) return fail(error, "transfer.cancelled");
            if (response.failed) return fail(error, "github.response_error");
            if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE) return fail(error, "github.tls_error");
            return fail(error, "github.network_error");
        }
        return true;
    }
    bool httpOk(long status, std::string& error)
    {
        if (status >= 200 && status < 300) return true;
        if (status == 401 || status == 403) return fail(error, "github.auth_error");
        if (status == 409 || status == 422) return fail(error, "github.conflict");
        if (status == 404) return fail(error, "github.not_found");
        error = i18n::t("github.http_error", {std::to_string(status)}); return false;
    }
    bool checkRepository(const GitHubSync::Config& cfg, std::string& error)
    {
        Response r; long status = 0;
        if (!request(cfg, "GET", "https://api.github.com/repos/" + cfg.repository, r, status, error) || !httpOk(status, error)) return false;
        auto j = Json::parse(r.body, nullptr, false);
        if (!j.is_object() || !j.contains("private") || !j["private"].is_boolean()) return fail(error, "github.response_error");
        // An accidental public destination must never receive a save.
        if (!j["private"].get<bool>()) return fail(error, "github.private_required");
        return true;
    }
    bool collect(const std::string& root, const std::string& rel, std::vector<TransferProto::SendFile>& files,
        std::vector<std::string>& dirs, size_t& entries)
    {
        if (TransferStatus::cancelRequested() || rel.size() > 700) return false;
        std::unique_ptr<DIR, decltype(&closedir)> dir(opendir((root + rel).c_str()), closedir);
        if (!dir) return false;
        while (auto* entry = readdir(dir.get())) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (lower == "github-token.txt") return false;
            if (++entries > 4096) return false;
            const std::string relative = rel + name, full = root + relative;
            if (!TransferProto::isSafeZipRelativePath(relative)) return false;
            struct stat st;
            if (lstat(full.c_str(), &st) != 0) return false;
            if (S_ISDIR(st.st_mode)) {
                dirs.push_back(relative + "/");
                if (!collect(root, relative + "/", files, dirs, entries)) return false;
            }
            else if (S_ISREG(st.st_mode)) {
                if (st.st_size < 0 || (uint64_t)st.st_size > MAX_ARCHIVE) return false;
                files.push_back({full, relative, (uint32_t)st.st_size});
            }
            else return false;
        }
        return true;
    }
    struct ZipWriter : TransferProto::ByteSink {
        FILE* f;
        explicit ZipWriter(FILE* file) : f(file) {}
        bool sendAll(const void* p, size_t n) override { return fwrite(p, 1, n, f) == n; }
    };
    struct ZipReader : TransferProto::FileReader {
        File f;
        bool open(const std::string& path) override { f.reset(fopen(path.c_str(), "rb")); return !!f; }
        size_t read(void* p, size_t n) override { return f ? fread(p, 1, n, f.get()) : 0; }
        void close() override { f.reset(); }
    };
    struct ByteReader : TransferProto::ByteReader {
        FILE* f;
        explicit ByteReader(FILE* file) : f(file) {}
        size_t read(void* p, size_t n) override { return fread(p, 1, n, f); }
    };
    // Extract to an isolated new directory. Never write into a live save or overwrite
    // an existing backup. Bound untrusted ZIP output, even if headers claim more.
    struct Extractor : TransferProto::ExtractSink {
        std::string root;
        File f;
        uint64_t remaining = 0, total = 0;
        size_t entries = 0;
        explicit Extractor(std::string r) : root(std::move(r)) {}
        bool parents(const std::string& path) {
            if (path.size() > 700 || !TransferProto::isSafeZipRelativePath(path)) return false;
            for (size_t i = 0; i < path.size(); ++i) if (path[i] == '/') {
                const auto dir = root + path.substr(0, i);
                if (mkdir(dir.c_str(), 0777) != 0) { struct stat st; if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false; }
            }
            return true;
        }
        bool makeDir(const std::string& path) override { return ++entries <= 4096 && parents(path + "/"); }
        bool beginFile(const std::string& path, uint32_t n) override {
            if (++entries > 4096 || total + n > MAX_ARCHIVE || !parents(path)) return false;
            total += n; remaining = n;
            struct stat st;
            if (stat((root + path).c_str(), &st) == 0) return false;
            f.reset(fopen((root + path).c_str(), "wb")); return !!f;
        }
        bool writeFile(const void* p, size_t n) override {
            if (!f || n > remaining || fwrite(p, 1, n, f.get()) != n) return false;
            remaining -= n; return true;
        }
        void endFile() override {
            if (f && (remaining != 0 || fflush(f.get()) != 0 || ferror(f.get()))) broken = true;
            FILE* raw = f.release();
            if (raw && fclose(raw) != 0) broken = true;
        }
        bool broken = false;
    };
    void removeStaging(const std::string& root)
    {
        // Only receives the unique staging path created below, never remote input.
        std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(root.c_str()), closedir);
        if (dir) while (auto* entry = readdir(dir.get())) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            auto path = root + "/" + name;
            struct stat st;
            if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) removeStaging(path);
            else std::remove(path.c_str());
        }
        dir.reset(); rmdir(root.c_str());
    }
}

GitHubSync::Config GitHubSync::loadConfig()
{
    Config c;
    File f(fopen(CONFIG, "rb"));
    const std::string previous = std::string(CONFIG) + ".previous";
    if (!f) f.reset(fopen(previous.c_str(), "rb"));
    if (!f) return c;
    struct stat st;
    if (fstat(fileno(f.get()), &st) != 0 || st.st_size > 4096) return c;
    auto j = Json::parse(f.get(), nullptr, false);
    if (!j.is_object()) return c;
    if (j.contains("repository") && j["repository"].is_string()) c.repository = j["repository"].get<std::string>();
    if (j.contains("branch") && j["branch"].is_string()) c.branch = j["branch"].get<std::string>();
    if (j.contains("after_backup") && j["after_backup"].is_boolean()) c.afterBackup = j["after_backup"].get<bool>();
    return c;
}
bool GitHubSync::saveConfig(const Config& c)
{
    const auto data = Json{{"repository", c.repository}, {"branch", c.branch}, {"after_backup", c.afterBackup}}.dump(2);
    const std::string tmp = std::string(CONFIG) + ".tmp";
    File f(fopen(tmp.c_str(), "wb"));
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f.get()) == data.size();
    if (fclose(f.release()) != 0) ok = false;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    // FAT rename does not replace a file. Retain the previous version until
    // publication; loadConfig recovers it after an interrupted settings write.
    const std::string previous = std::string(CONFIG) + ".previous";
    struct stat st;
    const bool hadConfig = stat(CONFIG, &st) == 0;
    if (hadConfig) {
        std::remove(previous.c_str());
        if (std::rename(CONFIG, previous.c_str()) != 0) return false;
    }
    if (std::rename(tmp.c_str(), CONFIG) != 0) {
        if (hadConfig) std::rename(previous.c_str(), CONFIG);
        return false;
    }
    std::remove(previous.c_str());
    return true;
}
bool GitHubSync::tokenPresent() { return !readToken().empty(); }

bool GitHubSync::upload(const Config& cfg, const std::string& id, const std::string& kind,
    const std::u16string& backupPath, std::string& error)
{
    TempGuard clean;
    TransferStatus::beginNetwork(i18n::t("github.uploading"), 0);
    if (!checkRepository(cfg, error)) return false;
    std::string root = StringUtils::UTF16toUTF8(backupPath);
    while (!root.empty() && root.back() == '/') root.pop_back();
    const std::string name = root.substr(root.find_last_of('/') + 1) + ".zip";
    if (!safeArchiveName(name)) return fail(error, "github.name_error");
    root += '/';
    std::vector<TransferProto::SendFile> files;
    std::vector<std::string> dirs;
    size_t count = 0;
    if (!collect(root, "", files, dirs, count) || (files.empty() && dirs.empty())) return fail(error, "github.sd_error");
    auto size = TransferProto::zipStreamSize(files, dirs);
    if (!size || *size > MAX_ARCHIVE) return fail(error, "github.too_large");
    File zip(fopen(ZIP, "wb"));
    if (!zip) return fail(error, "github.sd_error");
    ZipWriter writer(zip.get()); ZipReader reader; bool cancelled = false;
    TransferStatus::setMode(i18n::t("github.packing"));
    TransferStatus::setBytes(0, *size);
    const bool packed = TransferProto::sendZipStream(writer, files, dirs, reader,
        [] { return TransferStatus::cancelRequested(); }, [](size_t n) { TransferStatus::addBytesDone(n); }, cancelled);
    const int zipClose = fclose(zip.release());
    if (!packed || zipClose != 0) return fail(error, cancelled ? "transfer.cancelled" : "github.sd_error");
    const auto url = contentsUrl(cfg, id, kind, name);
    Response metadata; long status = 0;
    if (!request(cfg, "GET", url + "?ref=" + encode(cfg.branch), metadata, status, error, nullptr, false, true)) return false;
    Json tail{{"branch", cfg.branch}};
    if (status == 200) {
        // Object media type supports archives over 1 MiB without inlining their bytes.
        auto j = Json::parse(metadata.body, nullptr, false);
        if (!j.is_object() || !j.contains("sha") || !j["sha"].is_string()) return fail(error, "github.response_error");
        tail["sha"] = j["sha"];
    }
    else if (status != 404) return httpOk(status, error);
    File body(fopen(BODY, "wb"));
    zip.reset(fopen(ZIP, "rb"));
    if (!body || !zip) return fail(error, "github.sd_error");
    const auto prefix = std::string("{\"message\":") + Json("Checkpoint backup " + id + " " + kind).dump() + ",\"content\":\"";
    bool ok = fwrite(prefix.data(), 1, prefix.size(), body.get()) == prefix.size();
    std::array<unsigned char, 12288> chunk;
    while (ok) {
        if (TransferStatus::cancelRequested()) return fail(error, "transfer.cancelled");
        const size_t n = fread(chunk.data(), 1, chunk.size(), zip.get());
        if (!n) { if (ferror(zip.get())) ok = false; break; }
        auto b64 = base64(chunk.data(), n);
        ok = fwrite(b64.data(), 1, b64.size(), body.get()) == b64.size();
    }
    const auto suffix = std::string("\",") + tail.dump().substr(1);
    ok = ok && fwrite(suffix.data(), 1, suffix.size(), body.get()) == suffix.size();
    if (fclose(body.release()) != 0) ok = false;
    zip.reset();
    if (!ok) return fail(error, "github.sd_error");
    TransferStatus::setMode(i18n::t("github.uploading"));
    Response uploaded;
    return request(cfg, "PUT", url, uploaded, status, error, BODY) && httpOk(status, error);
}

bool GitHubSync::list(const Config& cfg, const std::string& id, const std::string& kind,
    std::vector<RemoteBackup>& backups, std::string& error)
{
    TransferStatus::beginNetwork(i18n::t("github.listing"), 0);
    if (!checkRepository(cfg, error)) return false;
    Response r; long status = 0;
    if (!request(cfg, "GET", contentsUrl(cfg, id, kind) + "?ref=" + encode(cfg.branch), r, status, error)) return false;
    // Check the selected branch separately so an invalid branch is not mistaken for an empty folder.
    if (status == 404) {
        Response branch;
        return request(cfg, "GET", "https://api.github.com/repos/" + cfg.repository + "/branches/" + encode(cfg.branch), branch, status, error) && httpOk(status, error);
    }
    if (!httpOk(status, error)) return false;
    auto j = Json::parse(r.body, nullptr, false);
    if (!j.is_array()) return fail(error, "github.response_error");
    if (j.size() >= 1000) return fail(error, "github.list_limit");
    for (const auto& file : j) {
        if (!file.is_object() || !file.contains("name") || !file["name"].is_string() || !file.contains("sha") || !file["sha"].is_string() ||
            !file.contains("type") || file["type"] != "file") continue;
        auto name = file["name"].get<std::string>();
        if (safeArchiveName(name)) backups.push_back({std::move(name), file["sha"].get<std::string>()});
    }
    std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) { return a.name > b.name; });
    return true;
}

bool GitHubSync::download(const Config& cfg, const std::string& id, const std::string& kind,
    const RemoteBackup& backup, const std::u16string& rootPath, std::string& error)
{
    TempGuard clean;
    TransferStatus::beginNetwork(i18n::t("github.downloading"), 0);
    if (!safeArchiveName(backup.name)) return fail(error, "github.name_error");
    if (!checkRepository(cfg, error)) return false;
    File zip(fopen(ZIP, "wb"));
    if (!zip) return fail(error, "github.sd_error");
    Response r; r.file = zip.get(); r.limit = MAX_ARCHIVE; long status = 0;
    // Git blob SHA pins the selection even if another device changes the branch.
    const std::string url = "https://api.github.com/repos/" + cfg.repository + "/git/blobs/" + encode(backup.sha);
    if (!request(cfg, "GET", url, r, status, error, nullptr, true) || !httpOk(status, error)) return false;
    if (fclose(zip.release()) != 0 || !r.received) return fail(error, "github.sd_error");
    const auto root = StringUtils::UTF16toUTF8(rootPath);
    std::string stage, destination;
    bool created = false;
    for (unsigned i = 0; i < 1000; ++i) {
        const auto suffix = "-GitHub-" + std::to_string((unsigned long long)time(nullptr)) + "-" + std::to_string(i);
        destination = root + "/" + backup.name.substr(0, backup.name.size() - 4) + suffix;
        stage = root + "/.github-staging" + suffix;
        struct stat st;
        if (stat(destination.c_str(), &st) == 0 || stat(stage.c_str(), &st) == 0) continue;
        if (mkdir(stage.c_str(), 0777) == 0) { created = true; break; }
        return fail(error, "github.sd_error");
    }
    if (!created) return fail(error, "github.sd_error");
    bool extracted = false;
    unsigned expectedEntries = 0;
    {
        File input(fopen(ZIP, "rb"));
        std::array<unsigned char, 22> footer;
        if (!input || r.received < 22 || fseek(input.get(), -22, SEEK_END) != 0 ||
            fread(footer.data(), 1, footer.size(), input.get()) != footer.size() ||
            !validZipFooter(footer.data(), r.received, expectedEntries)) {
            removeStaging(stage);
            return fail(error, "github.extract_error");
        }
    }
    {
        zip.reset(fopen(ZIP, "rb"));
        Extractor sink(stage + "/");
        std::string detail;
        if (zip) {
            ByteReader reader(zip.get());
            TransferStatus::setMode(i18n::t("github.unpacking")); TransferStatus::setBytes(0, r.received);
            extracted = TransferProto::extractZip(reader, r.received, sink,
                [] { return TransferStatus::cancelRequested(); }, [](size_t n) { TransferStatus::addBytesDone(n); }, detail) && !sink.broken && sink.entries == expectedEntries;
        }
    }
    zip.reset();
    if (!extracted || TransferStatus::cancelRequested() || std::rename(stage.c_str(), destination.c_str()) != 0) {
        removeStaging(stage);
        return fail(error, TransferStatus::cancelRequested() ? "transfer.cancelled" : "github.extract_error");
    }
    return true;
}
