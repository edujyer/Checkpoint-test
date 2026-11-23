// Host tests for GitHub backup framing and failure handling.
#include "githubsync_core.hpp"
#include "transferprotocol.hpp"
#include <cassert>
#include <cstring>
#include <iostream>

struct Bytes : TransferProto::ByteSink, TransferProto::ByteReader {
    std::string data; size_t at = 0;
    bool sendAll(const void* p, size_t n) override { data.append((const char*)p, n); return true; }
    size_t read(void* p, size_t n) override { n = std::min(n, data.size() - at); memcpy(p, data.data() + at, n); at += n; return n; }
};
struct Source : TransferProto::FileReader {
    std::string data = "test-save"; size_t at = 0;
    bool open(const std::string&) override { at = 0; return true; }
    size_t read(void* p, size_t n) override { n = std::min(n, data.size() - at); memcpy(p, data.data() + at, n); at += n; return n; }
    void close() override {}
};
struct Destination : TransferProto::ExtractSink {
    std::string data; bool diskFull = false;
    bool makeDir(const std::string&) override { return true; }
    bool beginFile(const std::string&, uint32_t) override { return true; }
    bool writeFile(const void* p, size_t n) override { if (diskFull) return false; data.append((const char*)p, n); return true; }
    void endFile() override {}
};
int main()
{
    using namespace GitHubSync;
    assert(validRepository("edujyer/private-saves"));
    for (auto s : {"https://github.com/a/b", "a/b/c", "a/", "/b", "a/..", "a/b?x=1", "a/b\nAuthorization: leak"}) assert(!validRepository(s));
    assert(validBranch("feature/cloud"));
    for (auto s : {"", "..", "a..b", "a.lock", "a/.x", "a/../b", "a b", "a\nb", "main?", "a//b", "a/"}) assert(!validBranch(s));
    assert(safeArchiveName("2026-11-23 Pokemon.zip"));
    for (auto s : {"../secret.zip", "/x.zip", "x\\y.zip", "x:save.zip", ".zip", "x.zip/evil", "x. .zip", "x?.zip"}) assert(!safeArchiveName(s));
    const std::string plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const std::string encoded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (size_t i = 0; i < 7; ++i) assert(base64((const unsigned char*)plain[i].data(), plain[i].size()) == encoded[i]);
    std::string large(12289, 'x');
    const auto whole = base64((const unsigned char*)large.data(), large.size());
    assert(whole == base64((const unsigned char*)large.data(), 12288) + base64((const unsigned char*)large.data() + 12288, 1));
    Bytes zip; Source source; bool cancelled = false;
    assert(TransferProto::sendZipStream(zip, {{"save", "folder/save.bin", 9}}, {"folder/"}, source, [] { return false; }, {}, cancelled));
    unsigned entries = 0;
    assert(validZipFooter((const unsigned char*)zip.data.data() + zip.data.size() - 22, zip.data.size(), entries) && entries == 2);
    assert(!validZipFooter((const unsigned char*)zip.data.data() + zip.data.size() - 22, zip.data.size() - 1, entries));
    Destination sink; std::string error;
    assert(TransferProto::extractZip(zip, zip.data.size(), sink, {}, {}, error) && sink.data == source.data);
    zip.at = 0; Destination failed; failed.diskFull = true;
    assert(!TransferProto::extractZip(zip, zip.data.size(), failed, {}, {}, error));
    zip.at = 0; zip.data[30 + 7 + 30 + 15] ^= 1; Destination corrupt;
    assert(!TransferProto::extractZip(zip, zip.data.size(), corrupt, {}, {}, error));
    assert(!TransferProto::isSafeZipRelativePath("../github-token.txt"));
    assert(!TransferProto::isSafeZipRelativePath("/github-token.txt"));
    std::cout << "GitHub sync host checks passed\n";
}
