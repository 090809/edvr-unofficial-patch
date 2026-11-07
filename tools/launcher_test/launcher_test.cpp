// launcher_test -- the launcher's update core, over fixtures nobody can arrange by hand.
//
// What it proves, and how it would fail if the code under test never ran: every
// check below reads a value the code produced (bytes extracted from a zip, a
// verdict on a signed manifest, a file in a cache folder, a header the server
// received), so a stubbed or skipped function turns checks red, and the loops
// over fixture rows assert how many rows they saw.
//
//   json         the strict parser: depth, trailing bytes, duplicate keys, escapes
//   signing      Python-signed manifests accepted by BCrypt, every tampering refused
//   shapes       every shape tools\sign_manifest.py rejects is rejected here too
//   versions     compareVersions
//   zip          inflate over stored, fixed and dynamic blocks; corrupt and bomb input
//   cache        only missing assets fetched, tampering refused before the zip opens
//   state        launcher.ini
//   updater      GitHub scripted: 304, 403, 429, 5xx, bad signature, tag, issued, ...
//   http         WinHttpClient against a loopback server thread in this process
//   payload      usePayloadFiles
//
// Nothing here touches the real %LOCALAPPDATA%\EDVR, a game folder or any host but
// 127.0.0.1: the updater sections use a scripted client, and the http section
// speaks only to a listener this process opens on the loopback address.
//
// Usage: launcher_test.exe <repo root> <fixtures dir> <scratch dir>
//   fixtures are written by tools\launcher_fixtures.py.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/installer/launcher_state.h"
#include "../../src/installer/payload.h"
#include "../../src/installer/probe.h"
#include "../../src/installer/update_cache.h"
#include "../../src/installer/update_http.h"
#include "../../src/installer/update_json.h"
#include "../../src/installer/update_keys.h"
#include "../../src/installer/update_manifest.h"
#include "../../src/installer/update_util.h"
#include "../../src/installer/update_zip.h"
#include "../../src/installer/updater.h"

using namespace edvr::installer;

namespace {

int g_checks = 0;
int g_fails = 0;
std::wstring g_fixtures;
std::wstring g_scratch;
std::string g_testKey, g_otherKey;
const char* g_testKeys[2] = {nullptr, nullptr};

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

void section(const char* name) { std::printf("[launcher_test] %s\n", name); }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---- files ---------------------------------------------------------------------

std::wstring P(const std::wstring& a, const std::wstring& b) { return joinPath(a, b); }
std::wstring P(const std::wstring& a, const std::string& b) { return joinPath(a, widenUtf8(b)); }

std::string readAll(const std::wstring& path) {
    std::string bytes;
    readFileBytes(path, &bytes, 1ull << 31, nullptr);
    return bytes;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool writeAll(const std::wstring& path, const std::string& data) {
    createDirectories(path.substr(0, path.find_last_of(L"\\/")));
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < data.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
        if (!WriteFile(f, data.data() + done, want, &wrote, nullptr) || wrote == 0) break;
        done += wrote;
    }
    CloseHandle(f);
    return done == data.size();
}

void rmtree(const std::wstring& dir) {
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(dir, L"*").c_str(), &find);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = find.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = P(dir, name);
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(full);
            else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &find));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

std::vector<std::string> names(const std::wstring& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(dir, L"*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(narrowUtf8(find.cFileName));
    } while (FindNextFileW(h, &find));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring fresh(const std::string& name) {
    const std::wstring dir = P(g_scratch, name);
    rmtree(dir);
    createDirectories(dir);
    return dir;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
        start = end + 1;
    }
    return out;
}

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t end = line.find(sep, start);
        out.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string sha(const std::string& bytes) { return sha256Bytes(bytes.data(), bytes.size()); }

const unsigned char* U(const std::string& s) { return reinterpret_cast<const unsigned char*>(s.data()); }

unsigned long u32At(const std::string& s, size_t pos) {
    return static_cast<unsigned long>(static_cast<unsigned char>(s[pos])) |
           (static_cast<unsigned long>(static_cast<unsigned char>(s[pos + 1])) << 8) |
           (static_cast<unsigned long>(static_cast<unsigned char>(s[pos + 2])) << 16) |
           (static_cast<unsigned long>(static_cast<unsigned char>(s[pos + 3])) << 24);
}

// ---- json ----------------------------------------------------------------------

void testJson() {
    section("json");
    JsonValue v;
    std::string err;
    check(parseJson("{\"a\": [1, -0, 2.5, true, null, \"x\"], \"b\": {\"c\": 1e2}}", &v, &err), "a plain document parses: " + err);
    check(v.isObject() && v.find("a") && v.find("a")->items.size() == 6, "the array has six elements");
    check(v.find("a")->items[0].isInt() && v.find("a")->items[0].integer == 1, "1 is an integer");
    check(v.find("a")->items[1].isInt() && v.find("a")->items[1].integer == 0, "-0 is the integer 0");
    check(v.find("a")->items[2].type == JsonValue::Type::Float, "2.5 is a float, not an integer");
    check(v.find("b")->find("c")->type == JsonValue::Type::Float, "1e2 is a float, not an integer");
    check(v.find("zz") == nullptr, "a missing key is null");

    check(parseJson("\"caf\\u00e9 \\ud83d\\ude00 \\\" \\\\ \\/\"", &v, &err), "escapes parse");
    check(v.text == "caf\xC3\xA9 \xF0\x9F\x98\x80 \" \\ /", "\\u escapes decode to UTF-8, pairs join");
    check(parseJson("\"\\ud800\"", &v, &err) && v.text == "\xED\xA0\x80", "a lone high surrogate is kept as its own three bytes, as Python does");
    check(parseJson("\"\\udc00x\"", &v, &err) && v.text == "\xED\xB0\x80x", "a lone low surrogate is kept as its own three bytes");
    check(parseJson("\"\\ud800\\u0041\"", &v, &err) && v.text == "\xED\xA0\x80" "A", "a high surrogate then a plain escape stay separate");
    check(!parseJson("\"\\ud800\\u00zz\"", &v, &err), "a bad escape after a high surrogate is still refused");
    check(!parseJson("\"\\x41\"", &v, &err), "an unknown escape is refused");
    check(!parseJson(std::string("\"a\tb\""), &v, &err), "a raw control character in a string is refused");
    check(!parseJson("\"\xC0\x80\"", &v, &err), "an overlong UTF-8 form is refused");
    check(!parseJson("\"\xED\xA0\x80\"", &v, &err), "an encoded surrogate is refused");
    check(!parseJson("\"\xC3\"", &v, &err), "a cut UTF-8 sequence is refused");
    check(parseJson("\"\xC3\xA9\"", &v, &err) && v.text == "\xC3\xA9", "valid UTF-8 passes through");

    check(!parseJson("{\"a\": 1, \"a\": 2}", &v, &err) && contains(err, "repeated"), "a repeated key is refused");
    check(!parseJson("{\"a\": 1, \"\\u0061\": 2}", &v, &err), "a repeated key written with an escape is refused");
    check(parseJson("[{\"a\":1},{\"a\":2}]", &v, &err), "the same key in two objects is fine");
    check(!parseJson("{} x", &v, &err) && contains(err, "follow"), "bytes after the value are refused");
    check(!parseJson("[] []", &v, &err), "a second value is refused");
    check(parseJson(" \t\r\n[ ] \n", &v, &err), "white space around the value is fine");
    check(!parseJson("", &v, &err), "empty input is refused");
    check(!parseJson("[1,]", &v, &err), "a trailing comma is refused");
    check(!parseJson("01", &v, &err), "a leading zero is refused");
    check(!parseJson("NaN", &v, &err), "NaN is refused");
    check(!parseJson("1.", &v, &err), "a number with no fraction digits is refused");
    check(!parseJson("-", &v, &err), "a bare minus is refused");
    check(!parseJson("9223372036854775808", &v, &err), "an integer past 64 bits is refused");
    check(parseJson("9223372036854775807", &v, &err) && v.integer == 9223372036854775807ll, "the largest integer is kept exactly");
    check(!parseJson("{'a': 1}", &v, &err), "single quotes are refused");
    check(!parseJson("[tru]", &v, &err), "a misspelt literal is refused");

    std::string deep32(32, '[');
    deep32 += std::string(32, ']');
    check(parseJson(deep32, &v, &err), "nesting of 32 is accepted");
    std::string deep33(33, '[');
    deep33 += std::string(33, ']');
    check(!parseJson(deep33, &v, &err) && contains(err, "deep"), "nesting of 33 is refused");
    std::string mixed;
    for (int i = 0; i < 33; ++i) mixed += (i % 2 ? "[" : "{\"k\":");
    mixed += "0";
    for (int i = 32; i >= 0; --i) mixed += (i % 2 ? "]" : "}");
    check(!parseJson(mixed, &v, &err), "33 levels of mixed arrays and objects are refused");
    check(v.type == JsonValue::Type::Null && v.items.empty(), "a refused parse leaves the output empty");
}

// ---- signing and keys ------------------------------------------------------------

bool loadWith(const char* const* keys, const std::string& bytes, const std::string& sig, Manifest* m, std::string* err) {
    return loadVerifiedManifest(bytes, sig, keys, m, err);
}

void testSigning() {
    section("signing");
    check(g_testKey.size() == 128 && g_otherKey.size() == 128, "the fixture keys are 128 hex characters");

    // The embedded keys: two, well formed, and never a test key.
    size_t embedded = 0;
    for (const char* const* k = kUpdatePublicKeys; *k; ++k) {
        ++embedded;
        std::string key = *k;
        check(key.size() == 128, "an embedded key is 128 characters");
        std::string lowerKey = key, lowerTest = g_testKey, lowerOther = g_otherKey;
        for (char& c : lowerKey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        check(lowerKey != lowerTest, "an embedded key is the RFC 6979 test key");
        check(lowerKey != lowerOther, "an embedded key is the rig's other key");
    }
    check(embedded == 2, "update_keys.h holds the main and the recovery key");

    const std::wstring dir = P(g_fixtures, L"signing");
    const auto rows = lines(readAll(P(g_fixtures, L"signing.txt")));
    check(rows.size() >= 20, "the signing fixture lists its cases");
    int accepted = 0, refused = 0;
    for (const std::string& row : rows) {
        const auto f = split(row, '\t');
        if (f.size() != 4) { check(false, "a signing row has four fields: " + row); continue; }
        Manifest m;
        std::string err;
        const bool ok = loadWith(g_testKeys, readAll(P(dir, f[1])), readAll(P(dir, f[2])), &m, &err);
        check(ok == (f[3] == "accept"), "signing case " + f[0] + " expected " + f[3] + (ok ? ", accepted" : ", refused: " + err));
        (ok ? accepted : refused)++;
    }
    check(accepted >= 3 && refused >= 15, "the signing cases include accepts and refusals");

    const std::string good = readAll(P(dir, L"good.json")), goodSig = readAll(P(dir, L"good.sig"));
    Manifest m;
    std::string err;
    check(loadWith(g_testKeys, good, goodSig, &m, &err), "the Python-signed manifest is accepted by BCrypt");
    check(m.version == "1.0.0" && m.tag == "v1.0.0" && m.build == "v1.0.0" && m.issued == 1794000000 &&
              m.minLauncher == "0.1.0" && !m.hasLauncher && m.assets.size() == 1 && m.files.size() == 3,
          "the accepted manifest's fields are read");
    check(m.files[0].item == "native_graphics" && m.files[0].asset == m.assets[0].name && m.files[0].sha256.size() == 64,
          "a file entry is read whole");

    // Either listed key may sign; a list with neither refuses; the real keys refuse a test-signed manifest.
    const char* second[3] = {g_otherKey.c_str(), g_testKey.c_str(), nullptr};
    check(loadWith(second, good, goodSig, &m, &err), "the second listed key may be the one that signed");
    const char* none[1] = {nullptr};
    check(!loadWith(none, good, goodSig, &m, &err), "an empty key list refuses everything");
    check(!loadWith(nullptr, good, goodSig, &m, &err), "a null key list refuses everything");
    check(!loadWith(kUpdatePublicKeys, good, goodSig, &m, &err), "the embedded keys refuse a test-signed manifest");
    const char* wrong[2] = {g_otherKey.c_str(), nullptr};
    check(!loadWith(wrong, good, goodSig, &m, &err), "a stranger's key does not verify the signature");
    const std::string notAKey(128, 'z'), shortKey(100, 'a');
    const char* junkThenGood[4] = {notAKey.c_str(), shortKey.c_str(), g_testKey.c_str(), nullptr};
    check(loadWith(junkThenGood, good, goodSig, &m, &err), "a key that is not a point is skipped, the next is tried");
    const char* offCurve[2] = {"0000000000000000000000000000000000000000000000000000000000000001"
                               "0000000000000000000000000000000000000000000000000000000000000001", nullptr};
    check(!loadWith(offCurve, good, goodSig, &m, &err), "a key that is not on the curve verifies nothing");

    // The signature is checked before anything is parsed: a bad signature over
    // text that is not JSON reports the signature, and a good one reports the JSON.
    check(!loadWith(g_testKeys, "not json {", goodSig, &m, &err) && contains(err, "signature") && !contains(err, "JSON"),
          "a bad signature is reported before the parser runs: " + err);
    const auto shapes = lines(readAll(P(g_fixtures, L"shapes.txt")));
    for (const std::string& row : shapes) {
        const auto f = split(row, '\t');
        if (f.size() == 2 && f[0].size() > 4 && f[0].substr(4) == "not-json") {
            const std::wstring base = P(P(g_fixtures, L"shapes"), f[0]);
            check(!loadWith(g_testKeys, readAll(base + L".json"), readAll(base + L".sig"), &m, &err) && contains(err, "JSON"),
                  "a good signature over non-JSON reaches the parser: " + err);
        }
    }
    check(loadWith(g_testKeys, good, goodSig, &m, &err) && m.files.size() == 3, "a refused load does not disturb a later one");
    Manifest untouched;
    untouched.version = "keep";
    check(!loadWith(g_testKeys, good + " ", goodSig, &untouched, &err) && untouched.version == "keep", "a refused load leaves the output alone");
}

// ---- schema shapes -----------------------------------------------------------------

void testShapes() {
    section("shapes");
    const auto rows = lines(readAll(P(g_fixtures, L"shapes.txt")));
    check(rows.size() > 120, "the shape fixture lists its cases");
    std::map<std::string, std::string> verdictByName;
    int accepted = 0, refused = 0, mismatched = 0;
    for (const std::string& row : rows) {
        const auto f = split(row, '\t');
        if (f.size() != 2) { check(false, "a shape row has two fields: " + row); continue; }
        const std::wstring base = P(P(g_fixtures, L"shapes"), f[0]);
        Manifest m;
        std::string err;
        const bool ok = loadVerifiedManifest(readAll(base + L".json"), readAll(base + L".sig"), g_testKeys, &m, &err);
        const bool pythonAccepts = f[1] == "accept";
        if (ok != pythonAccepts) ++mismatched;
        check(ok == pythonAccepts, "shape " + f[0] + ": sign_manifest.py says " + f[1] + ", the launcher " + (ok ? "accepts" : "refuses: " + err));
        (ok ? accepted : refused)++;
        verdictByName[f[0].substr(4)] = ok ? "accept" : "refuse";
    }
    check(mismatched == 0, "the C++ and Python validators agree on every shape");
    check(accepted >= 14 && refused >= 100, "the shapes include both accepted and refused manifests");
    for (const char* name : {"path-6", "path-14", "path-2", "path-9", "file-sha-not-hex", "asset-sha-upper", "asset-name-5", "asset-name-6",
                             "launcher-asset-dots", "bom", "non-ascii-raw", "duplicate-key", "item-unknown", "item-alias-d3d11"}) {
        check(verdictByName.count(name) && verdictByName[name] == "refuse", std::string("the launcher refuses ") + name);
    }
    for (const char* name : {"valid", "valid-launcher", "valid-escaped-version", "valid-escaped-path", "valid-minus-zero-size",
                             "path-nested-ok", "item-ini", "item-openxr_license"}) {
        check(verdictByName.count(name) && verdictByName[name] == "accept", std::string("the launcher accepts ") + name);
    }
    for (const char* item : kManifestItems) {
        check(verdictByName.count(std::string("item-") + item) && verdictByName[std::string("item-") + item] == "accept",
              std::string("a manifest naming item ") + item + " is accepted");
    }

    // A parsed shape keeps what the file said.
    const std::wstring base = P(P(g_fixtures, L"shapes"), L"001-valid-launcher");
    Manifest m;
    std::string err;
    check(loadVerifiedManifest(readAll(base + L".json"), readAll(base + L".sig"), g_testKeys, &m, &err) && m.hasLauncher &&
              m.launcher.asset == "ed-up-launcher.exe" && m.launcher.size == 1 && m.launcher.version == "0.1.0",
          "the launcher entry is read");
}

// ---- versions -------------------------------------------------------------------

void testVersions() {
    section("versions");
    struct Case { const char* a; const char* b; int sign; };
    const Case cases[] = {
        {"0.1.0", "0.1.0", 0}, {"0.1.0", "0.2.0", -1}, {"0.10.0", "0.9.0", 1}, {"1.0.0-rc.1", "1.0.0", -1},
        {"1.0.0", "1.0.0-rc.7", 1}, {"0.17.0-rc.6", "0.17.0-rc.7", -1}, {"0.17.0-rc.10", "0.17.0-rc.9", 1},
        {"v1.2.3", "1.2.3", 0}, {"1.2", "1.2.0", 0}, {"1.0.0-rc.7", "0.99.9", 1}, {"2.0.0", "1.99.99", 1},
        {"1.0.0-alpha", "1.0.0-1", 1}, {"1.0.0-rc", "1.0.0-rc.1", -1}, {"0.1.0", "0.1.1", -1}, {"10.0.0", "9.99.99", 1},
    };
    for (const Case& c : cases) {
        const int got = compareVersions(c.a, c.b);
        const int back = compareVersions(c.b, c.a);
        check((got < 0 ? -1 : got > 0 ? 1 : 0) == c.sign, std::string("compareVersions(") + c.a + ", " + c.b + ")");
        check((back < 0 ? -1 : back > 0 ? 1 : 0) == -c.sign, std::string("compareVersions(") + c.b + ", " + c.a + ") is the mirror");
    }
}

// ---- zip and inflate ----------------------------------------------------------------

struct InflateRow {
    std::string name;
    unsigned long long size;
    std::string sha;
    std::string kind;
};

void testZip() {
    section("zip");
    std::vector<InflateRow> rows;
    for (const std::string& line : lines(readAll(P(g_fixtures, L"inflate\\entries.txt")))) {
        const auto f = split(line, '\t');
        if (f.size() == 4) rows.push_back({f[0], std::strtoull(f[1].c_str(), nullptr, 10), f[2], f[3]});
    }
    check(rows.size() == 8, "the inflate fixture has eight entries");
    const std::wstring zipPath = P(g_fixtures, L"inflate\\inflate.zip");
    ZipReader zip;
    std::string err;
    check(zip.openFile(zipPath, &err), "the fixture zip opens from a file: " + err);
    check(zip.entryCount() == 8, "the directory lists eight entries");
    InflateStats stats;
    for (const InflateRow& row : rows) {
        std::vector<unsigned char> bytes;
        const bool ok = zip.extract(row.name, row.size, &bytes, &err, &stats);
        check(ok, "extract " + row.name + ": " + err);
        check(ok && bytes.size() == row.size && sha256Bytes(bytes.data(), bytes.size()) == row.sha,
              "the bytes of " + row.name + " match the hash Python recorded");
    }
    check(stats.storedBlocks >= 1, "inflate met a stored block");
    check(stats.fixedBlocks >= 1, "inflate met a fixed-Huffman block");
    check(stats.dynamicBlocks >= 3, "inflate met dynamic-Huffman blocks");
    check(zip.has("stored.bin") && !zip.has("STORED.BIN") && !zip.has("stored.bi") && !zip.has("../stored.bin"), "entries are found by exact name only");

    std::vector<unsigned char> out;
    check(!zip.extract("no-such-entry", 1, &out, &err), "an unknown entry fails");
    check(!zip.extract("stored.bin", 299, &out, &err) && !zip.extract("stored.bin", 301, &out, &err), "a size other than the recorded one fails");
    check(!zip.extract("dynamic.txt", 5, &out, &err), "a smaller expected size than the entry's fails");
    check(zip.extract("empty-stored.bin", 0, &out, &err) && out.empty(), "an empty stored entry extracts");
    check(zip.extract("empty-deflate.bin", 0, &out, &err) && out.empty(), "an empty deflated entry extracts");

    // The same archive from memory, then damaged in ways a hostile server could.
    const std::string original = readAll(zipPath);
    check(original.size() > 5000000, "the fixture zip was read whole");
    {
        ZipReader memory;
        std::vector<unsigned char> bytes;
        check(memory.openMemory(U(original), original.size(), &err) && memory.extract("fixed.txt", rows[3].size, &bytes, &err),
              "the zip opens and extracts from memory");
    }
    auto mutated = [&](const std::function<void(std::string&)>& change) {
        std::string copy = original;
        change(copy);
        return copy;
    };
    auto opens = [&](const std::string& bytes, std::string* why) {
        ZipReader r;
        return r.openMemory(U(bytes), bytes.size(), why);
    };
    const size_t eocd = original.size() - 22;
    check(u32At(original, eocd) == 0x06054b50ul, "the fixture has no zip comment");
    std::string why;
    check(!opens(original.substr(0, original.size() / 2), &why), "a truncated zip does not open");
    check(!opens(original.substr(0, 10), &why) && !opens(std::string(), &why), "a tiny file does not open");
    check(!opens(mutated([&](std::string& b) { b[eocd + 10] = '\xff'; b[eocd + 11] = '\xff'; b[eocd + 8] = '\xff'; b[eocd + 9] = '\xff'; }), &why) &&
              contains(why, "zip64"), "a zip64 marker is refused: " + why);
    check(!opens(mutated([&](std::string& b) { b[eocd + 4] = 1; }), &why) && contains(why, "disks"), "a multi-disk archive is refused: " + why);
    check(!opens(mutated([&](std::string& b) { b[eocd + 16] = '\xff'; b[eocd + 17] = '\xff'; b[eocd + 18] = '\xff'; b[eocd + 19] = '\xff'; }), &why),
          "a directory offset of 0xffffffff is refused");
    check(!opens(mutated([&](std::string& b) { b[eocd + 12] = '\xff'; b[eocd + 13] = '\xff'; b[eocd + 14] = '\x7f'; }), &why),
          "a directory size past the file is refused");
    check(!opens(mutated([&](std::string& b) { b[eocd + 20] = 5; }), &why), "a comment length that does not reach the end is refused");

    // The first entry is stored.bin: its local header is at 0 and its central header opens the directory.
    const size_t storedLocal = 0;
    const size_t header = u32At(original, eocd + 16);
    check(u32At(original, storedLocal) == 0x04034b50ul && original.compare(storedLocal + 30, 10, "stored.bin") == 0, "the stored entry's local header is first");
    check(u32At(original, header) == 0x02014b50ul && original.compare(header + 46, 10, "stored.bin") == 0, "its central header opens the directory");
    {
        std::string encrypted = mutated([&](std::string& b) { b[header + 8] = static_cast<char>(b[header + 8] | 1); });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(encrypted), encrypted.size(), &why), "an archive with one encrypted entry still opens");
        check(!r.extract("stored.bin", 300, &bytes, &why) && contains(why, "encrypted"), "the encrypted entry is refused: " + why);
        check(r.extract("fixed.txt", rows[3].size, &bytes, &why), "its neighbours still extract");
    }
    {
        std::string odd = mutated([&](std::string& b) { b[header + 10] = 9; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(odd), odd.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why) && contains(why, "method"),
              "an unsupported method is refused: " + why);
    }
    {
        std::string renamed = mutated([&](std::string& b) { b[storedLocal + 32] = 'X'; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(renamed), renamed.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why) && contains(why, "names another"),
              "a local header that names another file is refused: " + why);
    }
    {
        std::string outside = mutated([&](std::string& b) { b[header + 23] = '\x7f'; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(outside), outside.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why),
              "a compressed size past the file is refused");
    }
    {
        std::string flippedCrc = mutated([&](std::string& b) { b[header + 16] ^= 1; });
        ZipReader r;
        std::vector<unsigned char> bytes;
        check(r.openMemory(U(flippedCrc), flippedCrc.size(), &why) && !r.extract("stored.bin", 300, &bytes, &why) && contains(why, "checksum"),
              "a wrong CRC is refused: " + why);
    }

    // Corrupt and truncated deflate streams fail cleanly, and nothing is written
    // past the output buffer.
    const InflateRow* dynamicRow = nullptr;
    for (const InflateRow& r : rows) if (r.name == "dynamic.txt") dynamicRow = &r;
    const size_t nameAt = original.find("dynamic.txt");
    const unsigned long compressedSize = u32At(original, nameAt - 30 + 18);
    const size_t dataAt = nameAt + 11;
    check(dynamicRow && compressedSize > 1000 && dataAt + compressedSize < original.size(), "the dynamic entry's stream is located");
    {
        std::vector<unsigned char> buffer(dynamicRow->size + 64, 0xA5);
        std::string e;
        check(inflateRaw(U(original) + dataAt, compressedSize, buffer.data(), dynamicRow->size, nullptr, nullptr, &e), "the stream inflates directly: " + e);
        check(buffer[dynamicRow->size] == 0xA5 && buffer[dynamicRow->size + 63] == 0xA5, "inflate wrote nothing past the output size");
        std::fill(buffer.begin(), buffer.end(), static_cast<unsigned char>(0xA5));
        check(!inflateRaw(U(original) + dataAt, compressedSize / 2, buffer.data(), dynamicRow->size, nullptr, nullptr, &e), "half a stream fails");
        check(!inflateRaw(U(original) + dataAt, 0, buffer.data(), dynamicRow->size, nullptr, nullptr, &e), "no stream fails");
        check(!inflateRaw(U(original) + dataAt, compressedSize, buffer.data(), dynamicRow->size - 1, nullptr, nullptr, &e) && contains(e, "larger"),
              "an output buffer one byte short stops the stream: " + e);
        check(!inflateRaw(U(original) + dataAt, compressedSize, buffer.data(), dynamicRow->size + 1, nullptr, nullptr, &e) && contains(e, "shorter"),
              "an output buffer one byte long is not filled: " + e);
        check(buffer[dynamicRow->size + 1] == 0xA5, "the failed runs left the sentinel alone");
    }
    int failedFlips = 0, flips = 0, wrongSuccesses = 0;
    for (int i = 0; i < 60; ++i) {
        std::string copy = original;
        copy[dataAt + static_cast<size_t>(i) * (compressedSize / 60)] ^= static_cast<char>(1 << (i % 8));
        ZipReader r;
        std::vector<unsigned char> bytes;
        if (!r.openMemory(U(copy), copy.size(), &why)) continue;
        ++flips;
        if (r.extract("dynamic.txt", dynamicRow->size, &bytes, &why)) {
            if (sha256Bytes(bytes.data(), bytes.size()) != dynamicRow->sha) ++wrongSuccesses;
        } else {
            ++failedFlips;
        }
    }
    check(flips == 60 && failedFlips >= 45, "single-bit flips in the stream fail the extract, not crash it");
    check(wrongSuccesses == 0, "no flip yields success with different bytes");
    unsigned seed = 12345;
    int garbageSurvived = 0;
    for (int trial = 0; trial < 300; ++trial) {
        std::string noise(200 + trial, '\0');
        for (char& c : noise) { seed = seed * 1664525u + 1013904223u; c = static_cast<char>(seed >> 24); }
        std::vector<unsigned char> buffer(1064, 0xCC);
        std::string e;
        inflateRaw(U(noise), noise.size(), buffer.data(), 1000, nullptr, nullptr, &e);
        bool sentinel = true;
        for (size_t i = 1000; i < buffer.size(); ++i) sentinel = sentinel && buffer[i] == 0xCC;
        garbageSurvived += sentinel ? 1 : 0;
    }
    check(garbageSurvived == 300, "300 streams of noise never wrote past the output buffer");

    // The bomb: the archive records 1000 bytes and the stream inflates to 100 MB.
    {
        ZipReader bomb;
        std::vector<unsigned char> bytes;
        check(bomb.openFile(P(g_fixtures, L"inflate\\bomb.zip"), &err), "the bomb archive opens: " + err);
        const DWORD before = GetTickCount();
        check(!bomb.extract("bomb.bin", 1000, &bytes, &why) && contains(why, "larger"), "the bomb stops at the recorded size: " + why);
        check(bytes.empty(), "no bomb output is handed back");
        check(GetTickCount() - before < 20000, "the bomb is refused without inflating its 100 MB");
        check(!bomb.extract("bomb.bin", 100u << 20, &bytes, &why), "asking for the bomb's real size fails: the archive records 1000");
    }
    // A hand-made stream: a stored block that claims more than it holds.
    {
        const unsigned char bad[] = {0x01, 0x10, 0x00, 0xef, 0xff, 'a', 'b'};
        unsigned char out16[16];
        std::string e;
        check(!inflateRaw(bad, sizeof(bad), out16, 16, nullptr, nullptr, &e), "a stored block longer than its input fails");
        const unsigned char badLen[] = {0x01, 0x02, 0x00, 0x00, 0x00, 'a', 'b'};
        check(!inflateRaw(badLen, sizeof(badLen), out16, 2, nullptr, nullptr, &e) && contains(e, "length check"), "a stored block with a bad length check fails");
        const unsigned char reserved[] = {0x07};
        check(!inflateRaw(reserved, sizeof(reserved), out16, 1, nullptr, nullptr, &e) && contains(e, "unknown type"), "block type 3 fails");
        const unsigned char tooFar[] = {0x03, 0x02};  // fixed block: a match with nothing before it
        check(!inflateRaw(tooFar, sizeof(tooFar), out16, 16, nullptr, nullptr, &e), "a back-reference before the start fails");
    }
}

// ---- the cache ---------------------------------------------------------------------

struct Rel {
    std::wstring dir;
    Manifest manifest;
    std::string manifestBytes, sigText;
    std::string name;
};

Rel loadRel(const char* name) {
    Rel r;
    r.name = name;
    r.dir = P(P(g_fixtures, L"releases"), name);
    r.manifestBytes = readAll(P(r.dir, L"ed-up-manifest.json"));
    r.sigText = readAll(P(r.dir, L"ed-up-manifest.json.sig"));
    std::string err;
    if (!loadVerifiedManifest(r.manifestBytes, r.sigText, g_testKeys, &r.manifest, &err)) {
        check(false, std::string("release fixture ") + name + " loads: " + err);
    }
    return r;
}

using Tamper = std::function<void(const std::string& assetName, std::string* bytes)>;

AssetFetcher serveFrom(const Rel& rel, std::vector<std::string>* log, Tamper tamper = nullptr) {
    return [dir = rel.dir, log, tamper](const ManifestAsset& asset, const std::wstring& dest, std::string* error) {
        log->push_back(asset.name);
        std::string bytes;
        if (!readFileBytes(P(dir, asset.name), &bytes, 1ull << 30, nullptr)) {
            *error = "the fixture has no such asset";
            return false;
        }
        if (tamper) tamper(asset.name, &bytes);
        return writeAll(dest, bytes);
    };
}

bool onlyHashNames(const std::wstring& dir, size_t expected) {
    const auto found = names(dir);
    if (found.size() != expected) return false;
    for (const std::string& n : found) if (!isLowerHex(n, 64)) return false;
    return true;
}

void testCache() {
    section("cache");
    const Rel a = loadRel("relA"), b = loadRel("relB");
    std::string err;
    std::vector<std::string> log;

    // Fetch only what is missing.
    {
        const std::wstring root = P(fresh("cache1"), L"payload");
        PayloadCache cache(root);
        std::vector<std::string> fetched;
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err, &fetched), "a fresh cache fills: " + err);
        check(fetched.size() == 2 && log.size() == 2, "both assets were fetched into an empty cache");
        check(cache.hasAll(a.manifest), "every file of release A is cached and verified");
        check(onlyHashNames(root, 4), "the cache folder holds four files, named by hash, and no temporary");
        for (const ManifestFile& f : a.manifest.files) {
            check(cache.hasVerified(f.sha256, f.size) && readAll(cache.pathFor(f.sha256)).size() == f.size, "cached file " + f.path + " is whole");
        }
        log.clear();
        fetched.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err, &fetched) && log.empty() && fetched.empty(), "a second fill fetches nothing");
        check(cache.ensureFiles(b.manifest, serveFrom(b, &log), &err, &fetched), "release B fills: " + err);
        check(log.size() == 1 && log[0] == "ed-up-core-1.1.0.zip", "only B's changed asset was fetched; the shared DLSS zip was not");
        check(cache.hasAll(b.manifest) && cache.hasAll(a.manifest), "both releases are now complete");
        check(onlyHashNames(root, 7), "the shared DLSS file is stored once");

        // Offline reads.
        check(cache.hasVerified(a.manifest.files[0].sha256, a.manifest.files[0].size) && !cache.hasVerified(a.manifest.files[0].sha256, a.manifest.files[0].size + 1),
              "hasVerified checks the size");
        check(cache.pathFor("../x").empty() && cache.pathFor("").empty() && cache.pathFor(std::string(64, 'G')).empty() &&
                  cache.pathFor(std::string(64, 'A')).empty() && !cache.pathFor(std::string(64, 'a')).empty(),
              "only a lower-case 64-digit hash names a cache file");
        // A file changed on disk no longer verifies, and the next fill repairs it.
        const std::wstring victim = cache.pathFor(a.manifest.files[0].sha256);
        std::string damaged = readAll(victim);
        damaged[0] ^= 1;
        check(writeAll(victim, damaged) && !cache.hasVerified(a.manifest.files[0].sha256, a.manifest.files[0].size), "a damaged cached file is not trusted");
        log.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err) && log.size() == 1 && cache.hasAll(a.manifest), "a damaged file is re-fetched and repaired: " + err);

        // Prune keeps what is kept.
        writeAll(P(root, L"junk.tmp"), "x");
        writeAll(P(root, L"notes.txt"), "keep me");
        writeAll(P(root, std::string(64, 'c')), "an unreferenced cached file");
        cache.prune({b.manifest});
        check(!exists(P(root, L"junk.tmp")), "prune removes a leftover temporary");
        check(!exists(P(root, std::string(64, 'c'))), "prune removes an unreferenced hash-named file");
        check(exists(P(root, L"notes.txt")), "prune leaves a file that is not a hash alone");
        check(cache.hasAll(b.manifest) && !cache.hasAll(a.manifest), "prune keeps B's files and drops A's alone");
        int stillA = 0;
        for (const ManifestFile& f : a.manifest.files) stillA += exists(cache.pathFor(f.sha256)) ? 1 : 0;
        check(stillA == 1, "of A's four files only the one B shares (the DLSS file) remains");
        cache.prune({});
        check(names(root).size() == 1, "pruning with nothing kept empties the cache of cached files");
    }

    // A leftover temporary is removed at the start of a fill.
    {
        const std::wstring root = P(fresh("cache2"), L"payload");
        createDirectories(root);
        writeAll(P(root, L"asset-1.tmp"), "half a download");
        writeAll(P(root, L"file-abc.tmp"), "half a file");
        PayloadCache cache(root);
        log.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err), "a fill over leftovers works: " + err);
        check(onlyHashNames(root, 4), "the leftover temporaries are gone");
    }

    // A tampered asset is refused before its zip is opened.
    {
        const std::wstring root = P(fresh("cache3"), L"payload");
        PayloadCache cache(root);
        log.clear();
        // One byte inside the compressed data: the zip itself would still open.
        Tamper flip = [](const std::string& name, std::string* bytes) {
            if (name.find("core") != std::string::npos) (*bytes)[60] ^= 1;
        };
        std::vector<std::string> fetched;
        check(!cache.ensureFiles(a.manifest, serveFrom(a, &log, flip), &err, &fetched), "a tampered asset fails the fill");
        check(contains(err, "hash") && !contains(err, "checksum") && !contains(err, "invalid"), "it is refused on the asset's hash, before the zip is read: " + err);
        check(names(root).empty(), "nothing is left in the cache, not even a temporary");

        // Same for a wrong size, even with a matching start.
        log.clear();
        Tamper cut = [](const std::string& name, std::string* bytes) {
            if (name.find("dlss") != std::string::npos) bytes->resize(bytes->size() - 3);
        };
        check(!cache.ensureFiles(a.manifest, serveFrom(a, &log, cut), &err) && contains(err, "size"), "a truncated asset is refused on its size: " + err);
        // The core zip came first and was fine: its files are whole, and no partial file exists.
        const auto left = names(root);
        bool allWhole = true;
        for (const std::string& n : left) allWhole = allWhole && isLowerHex(n, 64) && sha256File(P(root, n)) == n;
        check(allWhole && left.size() == 3, "after a failure on the second asset, only whole verified files remain (the first asset's three)");
        // And the next honest fill completes it without refetching what is cached.
        log.clear();
        check(cache.ensureFiles(a.manifest, serveFrom(a, &log), &err) && log.size() == 1 && cache.hasAll(a.manifest), "a later honest fill fetches only the missing asset: " + err);
    }

    // A file inside a good zip that does not match the manifest.
    {
        const std::wstring root = P(fresh("cache4"), L"payload");
        PayloadCache cache(root);
        Manifest lie = a.manifest;
        lie.files[1].sha256 = std::string(64, 'e');
        log.clear();
        check(!cache.ensureFiles(lie, serveFrom(a, &log), &err) && contains(err, lie.files[1].path), "a member that does not match its listed hash fails: " + err);
        check(!exists(cache.pathFor(std::string(64, 'e'))), "nothing is stored under the hash the manifest wrongly promised");
        bool noTemp = true;
        for (const std::string& n : names(root)) noTemp = noTemp && n.size() >= 4 && n.compare(n.size() - 4, 4, ".tmp") != 0;
        check(noTemp, "no temporary file remains");
        Manifest wrongPath = a.manifest;
        wrongPath.files[0].path = "no/such/entry.dll";
        PayloadCache second(P(fresh("cache4b"), L"payload"));
        check(!second.ensureFiles(wrongPath, serveFrom(a, &log), &err) && contains(err, "no entry"), "a manifest path the zip lacks fails: " + err);
        Manifest wrongSize = a.manifest;
        wrongSize.files[0].size += 1;
        PayloadCache third(P(fresh("cache4c"), L"payload"));
        check(!third.ensureFiles(wrongSize, serveFrom(a, &log), &err) && contains(err, "size"), "a manifest size the zip disagrees with fails: " + err);
    }

    // A fetcher that fails leaves nothing behind.
    {
        const std::wstring root = P(fresh("cache5"), L"payload");
        PayloadCache cache(root);
        const AssetFetcher failing = [](const ManifestAsset&, const std::wstring& dest, std::string* error) {
            writeAll(dest, "half");
            *error = "the network went away";
            return false;
        };
        check(!cache.ensureFiles(a.manifest, failing, &err) && contains(err, "network"), "a failing fetch fails the fill with its reason: " + err);
        check(names(root).empty(), "a failed fetch leaves no file");
    }
}

// ---- launcher.ini ---------------------------------------------------------------------

void testState() {
    section("state");
    const std::wstring dir = fresh("state");
    const std::wstring path = P(dir, L"launcher.ini");
    LauncherState s;
    std::string err;
    check(s.load(path, &err) && s.channel() == "stable" && s.currentManifest().empty() && s.highestIssued() == 0, "a missing file is an empty state");
    s.setChannel("rc");
    s.setCurrentManifest(std::string(64, 'a'));
    s.raiseHighestIssued(1794000100);
    s.raiseHighestIssued(1794000000);
    check(s.highestIssued() == 1794000100, "highest_issued is never lowered");
    s.setMode(L"C:\\Games\\Elite", "flat");
    check(s.save(&err), "saving works: " + err);
    LauncherState t;
    check(t.load(path, &err) && t.channel() == "rc" && t.currentManifest() == std::string(64, 'a') && t.highestIssued() == 1794000100,
          "the keys round-trip");
    check(t.mode(L"C:\\Games\\Elite") == "flat" && t.mode(L"c:/games/elite/") == "flat" && t.mode(L"C:\\GAMES\\ELITE\\\\") == "flat",
          "a mode is found by folder, whatever the case or separators");
    check(t.mode(L"C:\\Games\\Other").empty(), "another folder has no mode");
    check(LauncherState::modeKey(L"C:\\Games\\Elite") == "mode.3d6b05c4d8ea02f1", "the mode key is the first 16 hex of the SHA-256 of the lower-cased folder");

    // Unknown lines survive, in place.
    check(writeAll(path, "# kept comment\nfuture_key=7\nchannel=stable\nweird line\nhighest_issued=5\n"), "the rig writes a hand-made file");
    LauncherState u;
    check(u.load(path, &err), "it loads");
    u.set("channel", "rc");
    u.raiseHighestIssued(9);
    u.set("new_key", "1");
    check(u.save(&err), "it saves");
    check(readAll(path) == "# kept comment\nfuture_key=7\nchannel=rc\nweird line\nhighest_issued=9\nnew_key=1\n", "unknown lines are preserved in place and new keys are appended: " + readAll(path));
    check(!contains(readAll(path), "\r"), "the file is written with plain line feeds");
    bool noTemp = true;
    for (const std::string& n : names(dir)) noTemp = noTemp && n == "launcher.ini";
    check(noTemp, "the atomic write left no temporary beside the file");
    LauncherState v;
    v.load(path, &err);
    v.set("channel", "nonsense");
    check(v.channel() == "stable", "an unknown channel reads as stable");
    v.setCurrentManifest("not-a-hash");
    check(v.currentManifest().empty(), "a malformed manifest hash reads as none");
    v.set("highest_issued", "12x");
    check(v.highestIssued() == 0, "a malformed highest_issued reads as zero");
    v.set("channel", "rc\nevil=1");
    check(v.get("evil").empty(), "a value cannot start a new line");
    LauncherState w;
    check(!w.load(dir, &err), "a path that is a folder does not load as a state file");
}

// ---- the scripted GitHub -----------------------------------------------------------------

struct Reply {
    bool transportOk = true;
    int status = 200;
    std::string body;
    std::string etag;
    std::string error;
};

class ScriptedHttp : public HttpClient {
public:
    std::map<std::string, Reply> replies;
    std::vector<HttpRequest> requests;

    HttpResponse get(const HttpRequest& request) override {
        requests.push_back(request);
        Reply reply;
        const auto found = replies.find(request.url);
        if (found != replies.end()) reply = found->second;
        else reply.status = 404;
        HttpResponse out;
        out.transportOk = reply.transportOk;
        out.status = reply.status;
        out.error = reply.error;
        out.etag = reply.etag;
        if (!reply.transportOk) return out;
        if (!reply.etag.empty() && reply.status == 200) {
            for (const auto& h : request.headers) {
                if (h.first == "If-None-Match" && h.second == reply.etag) out.status = 304;
            }
        }
        if (out.status != 200) return out;
        if (request.maxBytes && reply.body.size() > request.maxBytes) {
            out.transportOk = false;
            out.error = "the reply is larger than allowed";
            return out;
        }
        if (!request.toFile.empty()) {
            if (!writeAll(request.toFile, reply.body)) {
                out.transportOk = false;
                out.error = "cannot write";
            }
            if (request.progress) request.progress(reply.body.size(), reply.body.size());
        } else {
            out.body = reply.body;
        }
        out.bytes = reply.body.size();
        return out;
    }

    int count(const std::string& part) const {
        int n = 0;
        for (const auto& r : requests) n += contains(r.url, part) ? 1 : 0;
        return n;
    }
    bool sentHeader(size_t index, const std::string& name, const std::string& value) const {
        for (const auto& h : requests[index].headers) if (h.first == name && h.second == value) return true;
        return false;
    }
    bool hasHeader(size_t index, const std::string& name) const {
        for (const auto& h : requests[index].headers) if (h.first == name) return true;
        return false;
    }
};

const char* kApi = "https://api.example.invalid/repos/p";
const char* kDownloads = "https://dl.example.invalid/";

std::string q(const std::string& s) { return "\"" + s + "\""; }

struct ApiOptions {
    bool includeSig = true;
    bool digests = true;
    std::string tag;       // empty: the release's own tag
    bool draft = false;
    std::string odd;       // a digest string to use instead of "sha256:<hex>"
    bool onlyManifestAssets = false;
};

std::string releaseTag(const Rel& r) { return "v" + r.manifest.version; }

std::string releaseJson(const Rel& r, const ApiOptions& o = ApiOptions()) {
    std::string assets;
    auto add = [&](const std::string& name, const std::string& bytes, bool digest) {
        if (!assets.empty()) assets += ",";
        assets += "{\"name\":" + q(name) + ",\"browser_download_url\":" + q(kDownloads + r.name + "/" + name) +
                  ",\"size\":" + std::to_string(bytes.size());
        if (o.digests && digest) assets += ",\"digest\":" + q(o.odd.empty() ? "sha256:" + sha(bytes) : o.odd);
        else if (o.digests) assets += ",\"digest\":null";
        assets += "}";
    };
    add("ed-up-manifest.json", r.manifestBytes, true);
    if (o.includeSig) add("ed-up-manifest.json.sig", r.sigText, true);
    if (!o.onlyManifestAssets) {
        for (const ManifestAsset& a : r.manifest.assets) add(a.name, readAll(P(r.dir, a.name)), false);
        if (r.manifest.hasLauncher) add("ed-up-launcher.exe", readAll(P(r.dir, L"ed-up-launcher.exe")), false);
    }
    return "{\"url\":\"https://api.example.invalid/x\",\"tag_name\":" + q(o.tag.empty() ? releaseTag(r) : o.tag) +
           ",\"draft\":" + (o.draft ? "true" : "false") + ",\"prerelease\":false,\"body\":\"notes \\u00e9\",\"assets\":[" + assets + "]}";
}

void serveRelease(ScriptedHttp* http, const Rel& r) {
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(P(r.dir, L"*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Reply reply;
        reply.body = readAll(P(r.dir, find.cFileName));
        http->replies[kDownloads + r.name + "/" + narrowUtf8(find.cFileName)] = reply;
    } while (FindNextFileW(h, &find));
    FindClose(h);
}

UpdaterSettings settingsFor(const std::wstring& base) {
    UpdaterSettings s;
    s.launcherDir = P(base, L"launcher");
    s.cacheDir = P(base, L"payload");
    s.apiBase = kApi;
    s.keys = g_testKeys;
    s.launcherVersion = "0.1.0";
    s.launcherSha256 = std::string(64, '1');
    s.channel = "stable";
    return s;
}

std::string latestUrl() { return std::string(kApi) + "/releases/latest"; }
std::string listUrl() { return std::string(kApi) + "/releases?per_page=20"; }

Reply ok(const std::string& body, const std::string& etag = std::string()) {
    Reply r;
    r.body = body;
    r.etag = etag;
    return r;
}

Reply failing(int status) {
    Reply r;
    r.status = status;
    return r;
}

bool adopt(Updater* u, const UpdateCheck& c, std::string* err) {
    return u->downloadUpdate(c, nullptr, nullptr, err) && u->commitManifest(c, err);
}

void testUpdater() {
    section("updater");
    const Rel a = loadRel("relA"), b = loadRel("relB"), old = loadRel("relOld"), minRel = loadRel("relMin"), minLauncher = loadRel("relMinLauncher");
    std::string err;

    // A first check, a download, an adoption, and an up-to-date answer that costs one request.
    {
        const std::wstring base = fresh("up1");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        serveRelease(&http, a);
        Updater up(http, settingsFor(base));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Available, "release A is offered: " + c.message);
        check(c.signatureVerified && c.tag == "v1.0.0" && c.manifest.version == "1.0.0" && c.manifestSha256 == sha(a.manifestBytes), "the check carries the verified manifest");
        check(contains(c.message, "1.0.0") && !c.launcherNeedsUpdate, "the message names the version; A has no launcher entry");
        check(http.requests.size() == 3 && http.requests[0].url == latestUrl(), "the check made the API request and fetched the manifest and signature, nothing more");
        check(http.sentHeader(0, "Accept", "application/vnd.github+json") && http.sentHeader(0, "X-GitHub-Api-Version", "2022-11-28") && !http.hasHeader(0, "If-None-Match"),
              "the first request carries the GitHub headers and no If-None-Match");
        check(!exists(P(settingsFor(base).cacheDir, L"x")) && names(settingsFor(base).cacheDir).empty(), "checking downloaded no payload file");
        check(http.requests[1].maxBytes == (1u << 20) && http.requests[2].maxBytes == (1u << 20), "the manifest and signature are capped at 1 MB each");

        std::string progressNames;
        check(up.downloadUpdate(c, nullptr, [&](const std::string& name, unsigned long long got, unsigned long long) { progressNames += name + (got ? "+" : "0") + ";"; }, &err),
              "the download fills the cache: " + err);
        check(contains(progressNames, "ed-up-core-1.0.0.zip"), "progress names the asset being fetched");
        check(http.count("ed-up-core-1.0.0.zip") == 1 && http.count("ed-up-dlss") == 1, "each missing asset was fetched once");
        LauncherState before;
        before.load(P(settingsFor(base).launcherDir, L"launcher.ini"), &err);
        check(before.currentManifest().empty() && before.highestIssued() == 0, "downloading adopts nothing");
        check(up.commitManifest(c, &err), "the commit succeeds: " + err);
        LauncherState after;
        after.load(P(settingsFor(base).launcherDir, L"launcher.ini"), &err);
        check(after.currentManifest() == c.manifestSha256 && after.highestIssued() == 1794000000, "the commit records current_manifest and highest_issued");
        check(exists(P(P(settingsFor(base).launcherDir, L"manifests"), widenUtf8(c.manifestSha256) + L".json")) &&
                  exists(P(P(settingsFor(base).launcherDir, L"manifests"), widenUtf8(c.manifestSha256) + L".sig")),
              "the manifest and its signature are stored under the manifest's hash");

        const size_t before2 = http.requests.size();
        const UpdateCheck again = up.check();
        check(again.outcome == UpdateOutcome::UpToDate, "the same digest is up to date: " + again.message);
        check(http.requests.size() == before2 + 1, "up to date cost one API request and no download");
        check(http.sentHeader(before2, "If-None-Match", "\"etag-A\""), "the stored ETag was sent as If-None-Match");

        // A reply without a digest is downloaded and still recognised as the adopted one.
        ScriptedHttp bare;
        bare.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.digests = false; return o; }()));
        serveRelease(&bare, a);
        Updater up2(bare, settingsFor(base));
        const UpdateCheck noDigest = up2.check();
        check(noDigest.outcome == UpdateOutcome::UpToDate && bare.count("ed-up-manifest.json") == 2, "with no digest listed the manifest is downloaded and found to be the adopted one: " + noDigest.message);
        ScriptedHttp odd;
        odd.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.odd = "md5:00ff"; return o; }()));
        serveRelease(&odd, a);
        Updater up3(odd, settingsFor(base));
        check(up3.check().outcome == UpdateOutcome::UpToDate && odd.count("ed-up-manifest.json") == 2, "an odd digest means download anyway");
    }
    // 304 with a pending update, and 304 with nothing stored.
    {
        const std::wstring base = fresh("up2");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a), "\"etag-A\"");
        serveRelease(&http, a);
        Updater up(http, settingsFor(base));
        check(up.check().outcome == UpdateOutcome::Available, "first check offers A");
        const size_t n = http.requests.size();
        const UpdateCheck second = up.check();
        check(http.sentHeader(n, "If-None-Match", "\"etag-A\""), "the second check sent the stored ETag");
        check(second.outcome == UpdateOutcome::Available && second.tag == "v1.0.0", "a 304 reuses the stored reply, so an update seen but not installed is still offered: " + second.message);
        check(exists(P(settingsFor(base).launcherDir, L"api-stable.cache")), "the reply is stored in the launcher folder");

        ScriptedHttp lone;
        lone.replies[latestUrl()] = failing(304);
        Updater up2(lone, settingsFor(fresh("up2b")));
        const UpdateCheck c = up2.check();
        check(c.outcome == UpdateOutcome::Skipped, "a 304 with no stored reply is skipped: " + c.message);
    }
    // Statuses and transport failures never block.
    for (int status : {403, 429, 500, 502, 503, 404, 418}) {
        ScriptedHttp http;
        http.replies[latestUrl()] = failing(status);
        const std::wstring base = fresh("up3");
        Updater up(http, settingsFor(base));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Skipped && contains(c.message, std::to_string(status)), "status " + std::to_string(status) + " is skipped: " + c.message);
        check(!exists(P(settingsFor(base).launcherDir, L"launcher.ini")) && http.requests.size() == 1, "a skipped check writes no state and asks nothing more");
    }
    {
        ScriptedHttp http;
        Reply r;
        r.transportOk = false;
        r.error = "no route";
        http.replies[latestUrl()] = r;
        Updater up(http, settingsFor(fresh("up4")));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Skipped && contains(c.message, "no route"), "a transport failure is skipped: " + c.message);
        ScriptedHttp junk;
        junk.replies[latestUrl()] = ok("<html>not json</html>");
        Updater up2(junk, settingsFor(fresh("up4b")));
        check(up2.check().outcome == UpdateOutcome::Skipped, "a reply that is not JSON is skipped");
        ScriptedHttp wrongShape;
        wrongShape.replies[latestUrl()] = ok("[]");
        Updater up3(wrongShape, settingsFor(fresh("up4c")));
        check(up3.check().outcome == UpdateOutcome::Skipped, "a list where a release was expected is skipped");
    }
    // A release without the signed manifest is not offered; a broken one is refused.
    {
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.includeSig = false; return o; }()));
        serveRelease(&http, a);
        Updater up(http, settingsFor(fresh("up5")));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::UpToDate && http.count("ed-up-manifest") == 0, "a release with no .sig is not offered and nothing is downloaded: " + c.message);
        ScriptedHttp bare;
        bare.replies[latestUrl()] = ok("{\"tag_name\":\"v0.0.1\",\"assets\":[]}");
        Updater up2(bare, settingsFor(fresh("up5b")));
        check(up2.check().outcome == UpdateOutcome::UpToDate, "stable with no manifest assets is up to date");

        ScriptedHttp gone;
        gone.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&gone, a);
        gone.replies[std::string(kDownloads) + "relA/ed-up-manifest.json.sig"] = failing(404);
        Updater up3(gone, settingsFor(fresh("up5c")));
        check(up3.check().outcome == UpdateOutcome::Refused, "a .sig that is listed but cannot be fetched is refused");
        gone.replies[std::string(kDownloads) + "relA/ed-up-manifest.json.sig"] = failing(503);
        check(up3.check().outcome == UpdateOutcome::Skipped, "a .sig that fails with a server error is skipped");

        ScriptedHttp badSig;
        badSig.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&badSig, a);
        // The manifest of A with the signature of B.
        badSig.replies[std::string(kDownloads) + "relA/ed-up-manifest.json.sig"] = ok(b.sigText);
        Updater up4(badSig, settingsFor(fresh("up5d")));
        const UpdateCheck c4 = up4.check();
        check(c4.outcome == UpdateOutcome::Refused, "a signature of another manifest is refused: " + c4.message);
        // GitHub's digest catches a swapped manifest before the signature does.
        ScriptedHttp swapped;
        swapped.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&swapped, a);
        swapped.replies[std::string(kDownloads) + "relA/ed-up-manifest.json"] = ok(b.manifestBytes);
        Updater up5(swapped, settingsFor(fresh("up5e")));
        const UpdateCheck c5 = up5.check();
        check(c5.outcome == UpdateOutcome::Refused && contains(c5.message, "digest"), "a manifest that is not the one GitHub lists is refused: " + c5.message);
        // Without a digest the signature alone refuses a flipped byte.
        std::string flipped = a.manifestBytes;
        flipped[flipped.find("1794000000") + 9] = '1';
        ScriptedHttp flippedHttp;
        flippedHttp.replies[latestUrl()] = ok(releaseJson(a, [] { ApiOptions o; o.digests = false; return o; }()));
        serveRelease(&flippedHttp, a);
        flippedHttp.replies[std::string(kDownloads) + "relA/ed-up-manifest.json"] = ok(flipped);
        Updater up6(flippedHttp, settingsFor(fresh("up5f")));
        const UpdateCheck c6 = up6.check();
        check(c6.outcome == UpdateOutcome::Refused && contains(c6.message, "signature"), "a flipped manifest byte is refused on the signature: " + c6.message);
        check(c6.manifest.version.empty() && !c6.signatureVerified, "a refused check carries no manifest");
        // A manifest signed by someone else.
        ScriptedHttp stranger;
        stranger.replies[latestUrl()] = ok(releaseJson(a));
        serveRelease(&stranger, a);
        UpdaterSettings other = settingsFor(fresh("up5g"));
        const char* otherKeys[2] = {g_otherKey.c_str(), nullptr};
        other.keys = otherKeys;
        Updater up7(stranger, other);
        check(up7.check().outcome == UpdateOutcome::Refused, "a manifest signed by a key the exe does not carry is refused");
        UpdaterSettings production = settingsFor(fresh("up5h"));
        production.keys = nullptr;
        Updater up8(stranger, production);
        check(up8.check().outcome == UpdateOutcome::Refused, "with the embedded keys a test-signed release is refused");
    }
    // The tag must match the release it was fetched from.
    {
        ScriptedHttp http;
        ApiOptions o;
        o.tag = "v9.9.9";
        http.replies[latestUrl()] = ok(releaseJson(a, o));
        serveRelease(&http, a);
        Updater up(http, settingsFor(fresh("up6")));
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Refused && contains(c.message, "tag"), "a manifest moved onto another release is refused: " + c.message);
    }
    // issued only goes up.
    {
        const std::wstring base = fresh("up7");
        ScriptedHttp http;
        http.replies[latestUrl()] = ok(releaseJson(b));
        serveRelease(&http, b);
        serveRelease(&http, old);
        serveRelease(&http, a);
        Updater up(http, settingsFor(base));
        const UpdateCheck cb = up.check();
        check(cb.outcome == UpdateOutcome::Available && adopt(&up, cb, &err), "B is offered and adopted: " + err);
        http.replies[latestUrl()] = ok(releaseJson(old));
        const UpdateCheck co = up.check();
        check(co.outcome == UpdateOutcome::UpToDate && contains(co.message, "older"), "an older verified manifest is ignored with a note: " + co.message);
        http.replies[latestUrl()] = ok(releaseJson(a));
        check(up.check().outcome == UpdateOutcome::UpToDate, "release A, issued before B, is ignored too");
        LauncherState s;
        s.load(P(settingsFor(base).launcherDir, L"launcher.ini"), &err);
        check(s.highestIssued() == 1794000100, "highest_issued still reads B's");
    }
    // The rc channel walks the list.
    {
        ScriptedHttp http;
        ApiOptions draft;
        draft.draft = true;
        const std::string noManifest = "{\"tag_name\":\"v2.0.0-rc.1\",\"draft\":false,\"assets\":[{\"name\":\"ed-up-launcher.exe\",\"browser_download_url\":\"https://dl.example.invalid/x\",\"size\":1}]}";
        // newest first: a draft that holds both assets, a release with none, then B, then A.
        http.replies[listUrl()] = ok("[" + releaseJson(minRel, draft) + "," + noManifest + "," + releaseJson(b) + "," + releaseJson(a) + "]");
        serveRelease(&http, b);
        serveRelease(&http, a);
        UpdaterSettings s = settingsFor(fresh("up8"));
        s.channel = "rc";
        Updater up(http, s);
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Available && c.tag == "v1.1.0", "rc takes the first non-draft release holding both assets, walking past one without: " + c.message);
        check(http.requests[0].url == listUrl() && http.count("relMin") == 0, "rc asked for the release list and never touched the draft");
        check(c.launcherNeedsUpdate, "B carries a newer launcher");
        // Channel from launcher.ini when the settings leave it empty.
        const std::wstring base = fresh("up8b");
        createDirectories(settingsFor(base).launcherDir);
        writeAll(P(settingsFor(base).launcherDir, L"launcher.ini"), "channel=rc\n");
        UpdaterSettings fromState = settingsFor(base);
        fromState.channel.clear();
        ScriptedHttp http2;
        http2.replies[listUrl()] = ok("[" + noManifest + "]");
        Updater up2(http2, fromState);
        const UpdateCheck none = up2.check();
        check(none.outcome == UpdateOutcome::UpToDate && http2.requests[0].url == listUrl(), "with no release holding a manifest rc is up to date, and the channel came from launcher.ini: " + none.message);
        check(exists(P(fromState.launcherDir, L"api-rc.cache")) && !exists(P(fromState.launcherDir, L"api-stable.cache")), "each channel keeps its own reply cache");
    }
    // min_launcher and the launcher's digest.
    {
        ScriptedHttp http;
        serveRelease(&http, minRel);
        serveRelease(&http, minLauncher);
        serveRelease(&http, b);
        http.replies[latestUrl()] = ok(releaseJson(minRel));
        Updater up(http, settingsFor(fresh("up9")));
        const UpdateCheck refused = up.check();
        check(refused.outcome == UpdateOutcome::Refused && contains(refused.message, "newer launcher"), "min_launcher above ours with no launcher in the manifest is refused: " + refused.message);
        UpdaterSettings newer = settingsFor(fresh("up9b"));
        newer.launcherVersion = "0.3.0";
        Updater upNew(http, newer);
        check(upNew.check().outcome == UpdateOutcome::Available, "a launcher at min_launcher may take the release");
        http.replies[latestUrl()] = ok(releaseJson(minLauncher));
        const UpdateCheck viaNewLauncher = up.check();
        check(viaNewLauncher.outcome == UpdateOutcome::Available && viaNewLauncher.launcherNeedsUpdate, "min_launcher above ours with a newer launcher in the manifest is offered, launcher first");

        http.replies[latestUrl()] = ok(releaseJson(b));
        const std::string theirs = b.manifest.launcher.sha256;
        UpdaterSettings same = settingsFor(fresh("up9c"));
        same.launcherSha256 = theirs;
        Updater upSame(http, same);
        check(!upSame.check().launcherNeedsUpdate, "the same launcher digest needs no update");
        UpdaterSettings differ = settingsFor(fresh("up9d"));
        Updater upDiffer(http, differ);
        check(upDiffer.check().launcherNeedsUpdate, "a different digest with a higher version needs an update");
        UpdaterSettings higher = settingsFor(fresh("up9e"));
        higher.launcherVersion = "0.3.0";
        Updater upHigher(http, higher);
        const UpdateCheck lower = upHigher.check();
        check(lower.outcome == UpdateOutcome::Available && !lower.launcherNeedsUpdate, "a different digest with a lower version is not a launcher update");
        UpdaterSettings equal = settingsFor(fresh("up9f"));
        equal.launcherVersion = "0.2.0";
        Updater upEqual(http, equal);
        check(upEqual.check().launcherNeedsUpdate, "an equal version with a different digest is a launcher update");
    }
    // Commit needs the files; retention keeps three; offline load works; the payload follows.
    {
        const std::wstring base = fresh("up10");
        ScriptedHttp http;
        serveRelease(&http, a);
        http.replies[latestUrl()] = ok(releaseJson(a));
        UpdaterSettings s = settingsFor(base);
        s.launcherVersion = "0.5.0";
        Updater up(http, s);
        const UpdateCheck c = up.check();
        check(c.outcome == UpdateOutcome::Available, "A is offered");
        check(!up.commitManifest(c, &err) && contains(err, "cache"), "committing before the files are cached is refused: " + err);
        LauncherState st;
        st.load(P(s.launcherDir, L"launcher.ini"), &err);
        check(st.currentManifest().empty() && !exists(P(s.launcherDir, L"manifests")), "the refused commit stored nothing");
        UpdateCheck notAvailable;
        check(!up.downloadUpdate(notAvailable, nullptr, nullptr, &err) && !up.commitManifest(notAvailable, &err), "nothing downloads or commits without an offered update");

        const Rel* sequence[] = {&a, &b, &minRel, &minLauncher};
        std::string lastSha;
        for (const Rel* rel : sequence) {
            http.replies[latestUrl()] = ok(releaseJson(*rel));
            serveRelease(&http, *rel);
            const UpdateCheck step = up.check();
            check(step.outcome == UpdateOutcome::Available, "release " + rel->name + " is offered in turn: " + step.message);
            check(adopt(&up, step, &err), "release " + rel->name + " is adopted: " + err);
            lastSha = step.manifestSha256;
            Sleep(30);
        }
        const std::wstring manifests = P(s.launcherDir, L"manifests");
        int json = 0, sig = 0;
        for (const std::string& n : names(manifests)) {
            json += n.size() == 69 && n.compare(64, 5, ".json") == 0;
            sig += n.size() == 68 && n.compare(64, 4, ".sig") == 0;
        }
        check(json == 3 && sig == 3, "only the newest three manifests are kept");
        check(!exists(P(manifests, widenUtf8(sha(a.manifestBytes)) + L".json")) && exists(P(manifests, widenUtf8(sha(b.manifestBytes)) + L".json")),
              "the oldest was dropped and the next newest kept");
        PayloadCache cache(s.cacheDir);
        check(!cache.hasAll(a.manifest) && cache.hasAll(b.manifest) && cache.hasAll(minRel.manifest) && cache.hasAll(minLauncher.manifest), "the cache holds exactly what the kept manifests list");
        check(!exists(cache.pathFor(a.manifest.files[0].sha256)), "a file only the dropped manifest used is gone");

        // Offline: a client that fails everything.
        ScriptedHttp offline;
        Updater off(offline, s);
        Manifest loaded;
        std::vector<PayloadFile> files;
        check(off.loadCurrentManifest(&loaded, &files, &err), "the current manifest loads offline: " + err);
        check(offline.requests.empty(), "loading it made no request");
        check(loaded.version == "1.3.0" && files.size() == 4, "it is the last adopted manifest, with four files");
        for (const PayloadFile& f : files) check(exists(f.path) && sha256File(f.path) == f.sha256, "payload file " + f.item + " is cached and verified");
        // Damage: a changed stored manifest, a changed signature, a missing cache file.
        const std::wstring storedJson = P(manifests, widenUtf8(lastSha) + L".json");
        const std::string original = readAll(storedJson);
        std::string changed = original;
        changed[changed.find("1.3.0")] = '9';
        writeAll(storedJson, changed);
        check(!off.loadCurrentManifest(&loaded, &files, &err) && contains(err, "changed"), "a stored manifest that no longer matches its hash is refused: " + err);
        writeAll(storedJson, original);
        const std::wstring storedSig = P(manifests, widenUtf8(lastSha) + L".sig");
        const std::string origSig = readAll(storedSig);
        writeAll(storedSig, std::string(128, '0') + "\n");
        check(!off.loadCurrentManifest(&loaded, &files, &err) && contains(err, "signature"), "a stored signature that no longer verifies is refused: " + err);
        writeAll(storedSig, origSig);
        check(off.loadCurrentManifest(&loaded, &files, &err), "restored, it loads again: " + err);
        DeleteFileW(cache.pathFor(minLauncher.manifest.files[0].sha256).c_str());
        check(!off.loadCurrentManifest(&loaded, &files, &err) && contains(err, "cache lacks"), "a missing cache file is named: " + err);
        std::vector<std::string> refetched;
        check(cache.ensureFiles(minLauncher.manifest, serveFrom(minLauncher, &refetched), &err) && refetched.size() == 1, "the cache repairs itself from the release: " + err);
        check(off.loadCurrentManifest(&loaded, &files, &err), "after the repair the current manifest loads again: " + err);
        check(usePayloadFiles(files, loaded.version, &err), "the payload switches to those files: " + err);
        const void* data = nullptr;
        size_t size = 0;
        check(payloadItem("native_graphics", &data, &size) && std::string(static_cast<const char*>(data), size) == "graphics four", "payloadItem now serves the cached graphics file");
        check(payloadInfo().version == "1.3.0" && payloadInfo().haveD3d11 && payloadInfo().haveNgx && payloadInfo().haveIni, "payloadInfo follows the manifest's version and files");
        resetPayloadFiles();

        const std::wstring emptyBase = fresh("up10b");
        Updater none(offline, settingsFor(emptyBase));
        check(!none.loadCurrentManifest(&loaded, &files, &err) && contains(err, "no update"), "with nothing adopted there is nothing to load: " + err);
    }
}

// ---- payload ---------------------------------------------------------------------------

void testPayload() {
    section("payload");
    const std::wstring dir = fresh("payload");
    const PayloadInfo embedded = payloadInfo();
    const void* data = nullptr;
    size_t size = 0;
    check(!payloadItem("ini", &data, &size) && !embedded.haveIni && !embedded.haveD3d11, "with no call made the rig's (resource-less) executable serves nothing, as before");
    const std::string baseVersion = embedded.version;
    const PayloadInfo* reference = &payloadInfo();

    auto make = [&](const wchar_t* leaf, const std::string& bytes) {
        const std::wstring path = P(dir, leaf);
        writeAll(path, bytes);
        return path;
    };
    const std::string graphics = "graphics bytes", ini = "[fix]\nx=1\n", ngx = "ngx bytes";
    std::string err;
    std::vector<PayloadFile> files = {{"d3d11", make(L"g.bin", graphics), sha(graphics)},
                                      {"ini", make(L"i.bin", ini), sha(ini)},
                                      {"ngx", make(L"n.bin", ngx), std::string()}};
    check(usePayloadFiles(files, "9.9.9", &err), "mapped files are accepted: " + err);
    check(payloadItem("native_graphics", &data, &size) && std::string(static_cast<const char*>(data), size) == graphics, "payloadItem serves the mapped file under its native name");
    const void* alias = nullptr;
    size_t aliasSize = 0;
    check(payloadItem("d3d11", &alias, &aliasSize) && alias == data && aliasSize == size, "the d3d11 alias serves the same bytes");
    check(!payloadItem("openvr", &data, &size) && !payloadItem("native_runtime", &data, &size), "an item the payload lacks is not served from anywhere else");
    check(!payloadItem("bogus", &data, &size), "an unknown item is not served");
    const PayloadInfo& info = payloadInfo();
    check(&info == reference, "payloadInfo hands back the same object, now updated");
    check(info.version == "9.9.9" && info.haveD3d11 && info.d3d11Sha == sha(graphics) && info.haveIni && info.iniText == ini && info.haveNgx && info.ngxSha == sha(ngx),
          "payloadInfo follows the files: version, hashes and the ini text");
    check(!info.haveOpenvr && !info.nativePairValid && !info.nativeGraphicsValid, "what is absent or not a valid native image reads absent or invalid");

    // The pointer handed out earlier outlives a replacement.
    check(payloadItem("ini", &data, &size), "the ini is served");
    const char* oldIni = static_cast<const char*>(data);
    const size_t oldIniSize = size;
    const std::string newIni = "[fix]\nx=2\n";
    check(usePayloadFiles({{"ini", make(L"i2.bin", newIni), sha(newIni)}}, "9.9.10", &err), "the payload can be replaced: " + err);
    check(std::string(oldIni, oldIniSize) == ini, "a pointer from before the replacement still reads the old bytes");
    check(payloadItem("ini", &data, &size) && std::string(static_cast<const char*>(data), size) == newIni && payloadInfo().iniText == newIni && payloadInfo().version == "9.9.10",
          "after the replacement the new bytes and version are served");
    check(!payloadItem("native_graphics", &data, &size) && !payloadInfo().haveD3d11, "the replaced payload no longer has the old items");

    // Refusals change nothing.
    check(!usePayloadFiles({{"ini", make(L"i3.bin", "other"), sha(ini)}}, "bad", &err) && contains(err, "hash"), "a wrong hash is refused: " + err);
    check(!usePayloadFiles({{"bogus", make(L"b.bin", "x"), std::string()}}, "bad", &err) && contains(err, "unknown"), "an unknown item is refused: " + err);
    check(!usePayloadFiles({{"d3d11", make(L"d1.bin", "x"), std::string()}, {"native_graphics", make(L"d2.bin", "y"), std::string()}}, "bad", &err) && contains(err, "twice"),
          "an alias and its native name together are refused as a repeat: " + err);
    check(!usePayloadFiles({{"ini", P(dir, L"missing.bin"), std::string()}}, "bad", &err) && contains(err, "cannot be opened"), "a missing file is refused: " + err);
    check(!usePayloadFiles({{"ini", make(L"empty.bin", ""), std::string()}}, "bad", &err), "an empty file is refused");
    check(payloadInfo().version == "9.9.10" && payloadItem("ini", &data, &size) && std::string(static_cast<const char*>(data), size) == newIni, "after the refusals the previous payload is intact");

    // Every item a manifest may name is one the payload knows.
    std::vector<PayloadFile> all;
    int index = 0;
    for (const char* item : kManifestItems) {
        const std::string bytes = std::string("bytes of ") + item;
        all.push_back({item, make((L"all" + std::to_wstring(index++) + L".bin").c_str(), bytes), sha(bytes)});
    }
    check(usePayloadFiles(all, "all", &err), "every manifest item is accepted by the payload: " + err);
    for (const char* item : kManifestItems) {
        check(payloadItem(item, &data, &size) && std::string(static_cast<const char*>(data), size) == std::string("bytes of ") + item, std::string("item ") + item + " is served");
    }
    check(payloadInfo().haveXess && payloadInfo().haveOpenxrLoader && payloadInfo().haveOpenxrLicense && payloadInfo().haveFlatIni && !payloadInfo().descriptorText.empty(),
          "payloadInfo reads the XeSS, loader, license, flat ini and descriptor items too");

    // No files is the empty payload; reset returns to the executable's own.
    check(usePayloadFiles({}, "none", &err), "an empty payload is allowed: " + err);
    check(!payloadItem("ini", &data, &size) && !payloadInfo().haveIni && payloadInfo().version == "none", "an empty payload serves nothing and says its version");
    resetPayloadFiles();
    check(payloadInfo().version == baseVersion && !payloadInfo().haveIni && &payloadInfo() == reference, "reset returns to the embedded behaviour");
}

// ---- WinHttpClient against a loopback server ----------------------------------------------------

std::string requestHeader(const std::string& request, const std::string& name) {
    std::string lowerReq = request, lowerName = name;
    for (char& c : lowerReq) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t at = lowerReq.find("\r\n" + lowerName + ":");
    if (at == std::string::npos) return std::string();
    size_t start = at + 2 + lowerName.size() + 1;
    while (start < request.size() && request[start] == ' ') ++start;
    const size_t end = request.find("\r\n", start);
    return request.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

class TestServer {
public:
    unsigned short port = 0;
    std::atomic<int> connections{0};
    std::atomic<bool> slowAborted{false};

    bool start() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return false;
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        int len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port = ntohs(addr.sin_port);
        if (listen(listener_, 16) != 0) return false;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void shutdown() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        listener_ = INVALID_SOCKET;
        WSACleanup();
    }

    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }

    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void run() {
        while (!stop_) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval wait = {0, 100000};
            if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
            SOCKET s = accept(listener_, nullptr, nullptr);
            if (s == INVALID_SOCKET) continue;
            ++connections;
            DWORD timeout = 5000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            handle(s);
            shutdown_(s);
        }
    }

    static void shutdown_(SOCKET s) {
        ::shutdown(s, SD_SEND);
        closesocket(s);
    }

    static bool sendAll(SOCKET s, const char* data, size_t size) {
        size_t done = 0;
        while (done < size) {
            const int n = send(s, data + done, static_cast<int>(std::min<size_t>(size - done, 1 << 20)), 0);
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
        }
        return true;
    }

    static bool reply(SOCKET s, int status, const char* text, const std::string& headers, const std::string& body, bool withLength = true) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\n" + headers;
        if (withLength) head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        head += "Connection: close\r\n\r\n";
        return sendAll(s, head.data(), head.size()) && sendAll(s, body.data(), body.size());
    }

    void handle(SOCKET s) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int n = recv(s, buffer, sizeof(buffer), 0);
            if (n <= 0) return;
            request.append(buffer, static_cast<size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }
        const size_t sp1 = request.find(' ');
        const size_t sp2 = request.find(' ', sp1 + 1);
        std::string path = request.substr(sp1 + 1, sp2 - sp1 - 1);
        const size_t query = path.find('?');
        if (query != std::string::npos) path.erase(query);
        const std::string self = "http://127.0.0.1:" + std::to_string(port);

        if (path == "/ok") {
            if (requestHeader(request, "If-None-Match") == "\"v1\"") { reply(s, 304, "Not Modified", "ETag: \"v1\"\r\n", ""); return; }
            reply(s, 200, "OK", "ETag: \"v1\"\r\n", "hello");
        } else if (path == "/redir") {
            reply(s, 302, "Found", "Location: /ok\r\n", "");
        } else if (path == "/redir-rel") {
            reply(s, 302, "Found", "Location: ok\r\n", "");
        } else if (path == "/redir-abs") {
            reply(s, 301, "Moved", "Location: " + self + "/ok\r\n", "");
        } else if (path == "/redir-307") {
            reply(s, 307, "Temporary Redirect", "Location: /redir\r\n", "");
        } else if (path == "/redir-loop") {
            reply(s, 302, "Found", "Location: /redir-loop\r\n", "");
        } else if (path == "/redir-plain") {
            reply(s, 302, "Found", "Location: http://example.com/x\r\n", "");
        } else if (path == "/redir-localhost") {
            reply(s, 302, "Found", "Location: http://localhost:" + std::to_string(port) + "/ok\r\n", "");
        } else if (path == "/redir-none") {
            reply(s, 302, "Found", "", "");
        } else if (path == "/404") {
            reply(s, 404, "Not Found", "", "nothing here");
        } else if (path == "/big") {
            reply(s, 200, "OK", "", std::string(100000, 'x'));
        } else if (path == "/big-chunked") {
            reply(s, 200, "OK", "", std::string(100000, 'x'), false);
        } else if (path == "/short") {
            // Promises 5000 bytes, sends 100, and closes.
            std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\nConnection: close\r\n\r\n" + std::string(100, 'y');
            sendAll(s, head.data(), head.size());
        } else if (path == "/file") {
            std::string body(300000, '\0');
            for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + (i * 7) % 26);
            reply(s, 200, "OK", "", body);
        } else if (path == "/slow") {
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 4194304\r\nConnection: close\r\n\r\n";
            if (!sendAll(s, head.data(), head.size())) { slowAborted = true; return; }
            const std::string chunk(65536, 's');
            for (int i = 0; i < 64; ++i) {
                if (!sendAll(s, chunk.data(), chunk.size())) { slowAborted = true; return; }
                Sleep(20);
            }
        } else if (path == "/echo") {
            reply(s, 200, "OK", "", request);
        } else {
            reply(s, 404, "Not Found", "", "");
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

void testHttp() {
    section("http");
    // The address rules, without a network.
    std::string why;
    check(urlAllowed("https://api.github.com/x", false, &why), "https is allowed");
    check(!urlAllowed("http://api.github.com/x", false, &why) && contains(why, "plain http"), "plain http is refused: " + why);
    check(!urlAllowed("http://api.github.com/x", true, &why), "plain http to another host is refused even for the rig's client");
    check(!urlAllowed("http://127.0.0.1:80/x", false, &why), "loopback http is refused by the product's client");
    check(urlAllowed("http://127.0.0.1:8080/x", true, &why), "loopback http is allowed for the rig's client");
    check(!urlAllowed("http://127.0.0.2/x", true, &why) && !urlAllowed("http://localhost/x", true, &why) && !urlAllowed("http://127.0.0.1.evil.com/x", true, &why) &&
              !urlAllowed("http://127.0.0.1@evil.com/x", true, &why),
          "only the literal 127.0.0.1 counts as loopback");
    check(!urlAllowed("ftp://x/y", true, &why) && !urlAllowed("file:///c:/x", true, &why) && !urlAllowed("//x/y", true, &why) && !urlAllowed("", true, &why), "other schemes are refused");
    check(!urlAllowed("https://user:pw@host/x", false, &why) && !urlAllowed("https://host:99999/x", false, &why) && !urlAllowed("https://ho st/x", false, &why),
          "user information, a bad port and a space are refused");
    check(redirectAllowed("https://a.example/x", "https://b.example/y", false, &why), "https to https may be followed");
    check(!redirectAllowed("https://a.example/x", "http://127.0.0.1:1/y", true, &why) && contains(why, "https to http"), "https down to http is refused, even to loopback: " + why);
    check(redirectAllowed("http://127.0.0.1:1/x", "https://b.example/y", true, &why), "http up to https may be followed");
    check(!redirectAllowed("http://127.0.0.1:1/x", "http://b.example/y", true, &why), "a redirect to plain http elsewhere is refused");
    check(resolveLocation("https://h.example/a/b?c=1", "/z") == "https://h.example/z", "an absolute path resolves against the origin");
    check(resolveLocation("https://h.example/a/b?c=1", "z") == "https://h.example/a/z", "a relative path resolves against the directory");
    check(resolveLocation("https://h.example/a/b", "http://o.example/q") == "http://o.example/q", "an absolute address stands");
    check(resolveLocation("https://h.example:8443/a/b", "//o.example/q") == "https://o.example/q", "a scheme-relative address takes the scheme");
    check(resolveLocation("https://h.example/a/b", "?x=1") == "https://h.example/a/b?x=1", "a query-only location keeps the path");

    TestServer server;
    check(server.start(), "the loopback server starts");
    if (server.port == 0) return;
    WinHttpClient client(true, "9.8.7");

    HttpRequest r;
    r.url = server.url("/ok");
    HttpResponse resp = client.get(r);
    check(resp.transportOk && resp.status == 200 && resp.body == "hello" && resp.etag == "\"v1\"" && resp.bytes == 5, "a 200 returns its body and ETag");
    r.headers = {{"If-None-Match", "\"v1\""}};
    resp = client.get(r);
    check(resp.transportOk && resp.status == 304 && resp.body.empty(), "a matching If-None-Match gets a 304 with no body");
    r.headers.clear();
    r.url = server.url("/404");
    resp = client.get(r);
    check(resp.transportOk && resp.status == 404 && resp.body.empty(), "a 404 is a status, not a transport failure");

    // Redirects.
    for (const char* path : {"/redir", "/redir-rel", "/redir-abs", "/redir-307"}) {
        r.url = server.url(path);
        resp = client.get(r);
        check(resp.transportOk && resp.status == 200 && resp.body == "hello", std::string("redirect ") + path + " is followed");
    }
    r.url = server.url("/redir-loop");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "too many redirects"), "a redirect loop stops: " + resp.error);
    r.url = server.url("/redir-none");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "no place"), "a redirect with no Location fails: " + resp.error);
    const int beforePlain = server.connections;
    r.url = server.url("/redir-plain");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "plain http"), "a redirect to a plain http address elsewhere is refused: " + resp.error);
    r.url = server.url("/redir-localhost");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "plain http"), "a redirect to localhost (not 127.0.0.1) is refused: " + resp.error);
    check(server.connections == beforePlain + 2, "each refused redirect cost only the one request that carried it");

    // Plain http is refused before any socket opens.
    {
        WinHttpClient strict(false);
        const int connectionsBefore = server.connections;
        HttpRequest direct;
        direct.url = server.url("/ok");
        resp = strict.get(direct);
        check(!resp.transportOk && contains(resp.error, "plain http") && server.connections == connectionsBefore, "the product's client refuses http to the loopback listener without connecting: " + resp.error);
        direct.url = "http://127.0.0.2:" + std::to_string(server.port) + "/ok";
        resp = client.get(direct);
        check(!resp.transportOk && server.connections == connectionsBefore, "the rig's client refuses http to another loopback address without connecting");
        direct.url = "http://example.com/";
        resp = client.get(direct);
        check(!resp.transportOk && contains(resp.error, "plain http"), "the rig's client refuses plain http to another host");
        direct.url = "ftp://example.com/";
        check(!client.get(direct).transportOk, "other schemes are refused");
    }

    // Size caps.
    r.url = server.url("/big");
    r.maxBytes = 1000;
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "larger") && resp.body.empty(), "a Content-Length past the cap fails early: " + resp.error);
    r.url = server.url("/big-chunked");
    resp = client.get(r);
    check(!resp.transportOk && contains(resp.error, "larger") && resp.body.empty(), "a body with no length is cut off at the cap: " + resp.error);
    r.maxBytes = 100000;
    r.url = server.url("/big");
    resp = client.get(r);
    check(resp.transportOk && resp.body.size() == 100000, "a body exactly at the cap is accepted");
    r.maxBytes = 0;
    r.url = server.url("/short");
    resp = client.get(r);
    check(!resp.transportOk, "a reply cut short of its Content-Length fails");

    // To a file, with progress.
    const std::wstring dir = fresh("http");
    {
        HttpRequest f;
        f.url = server.url("/file");
        f.toFile = P(dir, L"download.bin");
        unsigned long long lastGot = 0, lastTotal = 0;
        int calls = 0;
        bool monotonic = true;
        f.progress = [&](unsigned long long got, unsigned long long total) {
            monotonic = monotonic && got >= lastGot;
            lastGot = got;
            lastTotal = total;
            ++calls;
        };
        resp = client.get(f);
        const std::string saved = readAll(f.toFile);
        check(resp.transportOk && resp.status == 200 && resp.body.empty() && saved.size() == 300000 && resp.bytes == 300000, "a download lands in its file");
        check(saved[0] == 'a' && saved[1] == 'h' && saved[299999] == static_cast<char>('a' + (299999 * 7) % 26), "the file's bytes are the server's");
        check(calls >= 2 && monotonic && lastGot == 300000 && lastTotal == 300000, "progress was reported, rising to the total");
        f.maxBytes = 1000;
        f.toFile = P(dir, L"capped.bin");
        resp = client.get(f);
        check(!resp.transportOk && !exists(f.toFile), "a download past its cap leaves no file");
        f.maxBytes = 0;
        f.url = server.url("/404");
        f.toFile = P(dir, L"never.bin");
        resp = client.get(f);
        check(resp.transportOk && resp.status == 404 && !exists(f.toFile), "a 404 creates no file");
    }

    // Cancel mid-body.
    {
        std::atomic<bool> cancel{false};
        HttpRequest s;
        s.url = server.url("/slow");
        s.cancel = &cancel;
        s.toFile = P(dir, L"slow.bin");
        s.progress = [&](unsigned long long got, unsigned long long) { if (got >= 65536) cancel = true; };
        const DWORD began = GetTickCount();
        resp = client.get(s);
        check(!resp.transportOk && resp.cancelled && contains(resp.error, "cancel"), "a cancel stops the download: " + resp.error);
        check(GetTickCount() - began < 10000 && !exists(s.toFile), "it stopped well before the body ended and removed the partial file");
        for (int i = 0; i < 100 && !server.slowAborted; ++i) Sleep(50);
        check(server.slowAborted, "the server saw the connection close before the body was done");
        std::atomic<bool> already{true};
        HttpRequest pre;
        pre.url = server.url("/ok");
        pre.cancel = &already;
        const int connectionsBefore = server.connections;
        resp = client.get(pre);
        check(!resp.transportOk && resp.cancelled && server.connections == connectionsBefore, "a flag already set cancels before connecting");
    }

    // What a request says about the machine: only the user agent and the headers asked for.
    {
        HttpRequest e;
        e.url = server.url("/echo");
        e.headers = {{"Accept", "application/vnd.github+json"}};
        resp = client.get(e);
        const std::string seen = resp.body;
        check(resp.transportOk && contains(seen, "GET /echo HTTP/1.1"), "the echo returns the request as the server saw it");
        check(requestHeader(seen, "User-Agent") == "ed-up-launcher/9.8.7", "the user agent is ed-up-launcher/<version>: " + requestHeader(seen, "User-Agent"));
        check(requestHeader(seen, "Accept") == "application/vnd.github+json", "the caller's header went out");
        check(!contains(seen, "Authorization") && !contains(seen, "Cookie") && !contains(seen, "Referer") && !contains(seen, "Proxy-"), "no credential, cookie or referer is sent");
        char computer[256] = {};
        DWORD cn = sizeof(computer);
        if (GetComputerNameA(computer, &cn) && cn >= 4) {
            std::string lowerSeen = seen, lowerName = computer;
            for (char& c : lowerSeen) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            check(!contains(lowerSeen, lowerName), "the computer's name is not in the request");
        }
        char user[256] = {};
        DWORD un = sizeof(user);
        if (GetUserNameA(user, &un) && un >= 5) {
            std::string lowerSeen = seen, lowerUser = user;
            for (char& c : lowerSeen) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : lowerUser) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            check(!contains(lowerSeen, lowerUser.substr(0, un - 1)), "the user's name is not in the request");
        }
        WinHttpClient defaultAgent(true);
        resp = defaultAgent.get(e);
        check(requestHeader(resp.body, "User-Agent") == std::string("ed-up-launcher/") + EDUP_LAUNCHER_VERSION, "the default user agent carries the launcher's version");
    }
    server.shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: launcher_test <repo root> <fixtures dir> <scratch dir>\n");
        return 2;
    }
    g_fixtures = widenUtf8(argv[2]);
    g_scratch = widenUtf8(argv[3]);
    rmtree(g_scratch);
    if (!createDirectories(g_scratch)) {
        std::printf("launcher_test: cannot create the scratch folder\n");
        return 2;
    }
    {
        std::string key = readAll(P(g_fixtures, L"test_key.txt"));
        g_testKey = key;
        g_otherKey = readAll(P(g_fixtures, L"other_key.txt"));
        g_testKeys[0] = g_testKey.c_str();
        if (g_testKey.size() != 128 || g_otherKey.size() != 128) {
            std::printf("launcher_test: the fixtures are missing; run tools\\launcher_fixtures.py first\n");
            return 2;
        }
    }
    std::printf("launcher_test: fixtures %ls\n", g_fixtures.c_str());

    testJson();
    testSigning();
    testShapes();
    testVersions();
    testZip();
    testCache();
    testState();
    testUpdater();
    testPayload();
    testHttp();

    std::printf("\nlauncher_test: %d checks, %d failed\n", g_checks, g_fails);
    std::printf("%s\n", g_fails == 0 ? "launcher_test: all good" : "launcher_test: FAILURES");
    return g_fails == 0 ? 0 : 1;
}
