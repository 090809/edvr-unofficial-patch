#pragma once
// The concourse cull's pure half (docs\concourse-cull-2026-11-05.md): on foot in a large station's social space, the records that are not the room are
// not drawn. Nothing here touches Windows, the game or the log, so the optimized station's rig can test every rule. The engine half is in
// optimized_station.cpp (the judgement at the rig, the producer and the store) and concourse_pane.cpp (the panes).
//   concourseAllowName       the name allow-list: a prefix match, exact case, plus the temporary extra list (diag.concourse_keep)
//   concourseSocialOnFoot    the gate: Status.json Flags2 bit 0 (on foot) and bit 14 (social space), the file known, gameplay seen
//   concourseJudge           one record: keep (and why) or cull
//   concourseRigVetoed       one rig: vetoed only when a record of it was judged and none was kept
//   PaneTriple, paneTripleMatch, parsePaneList   a glass pane named by its vertex shader, pixel shader and index count
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr::concourse {

// The Flags2 bits (journal documentation): 0 OnFoot, 14 OnFootSocialSpace. Bit 3 (in station) rides with them in 0x00014009.
constexpr uint32_t kFlags2OnFoot = 1u << 0;
constexpr uint32_t kFlags2SocialSpace = 1u << 14;

// The anchor record's name: the large station's concourse. Its sighting by the store is the second half of the gate.
constexpr const char* kAnchorName = "Standard_SocialArea";
constexpr const char* kHubName = "SA_StationHub_Compact";

// The name prefixes that are the room. Exact case.
constexpr const char* kAllowPrefixes[] = {"Standard_SocialArea", "SA_",       "PS_",         "STAT_",        "FUI_",        "PROP_",
                                          "GUI_Interactive",     "Interactive_", "PlantStack", "TP_",         "HangarLobby"};
constexpr size_t kAllowPrefixCount = sizeof(kAllowPrefixes) / sizeof(kAllowPrefixes[0]);

inline bool hasPrefixExact(const char* name, const char* prefix, size_t n) noexcept { return n > 0 && std::strncmp(name, prefix, n) == 0; }

// True when `name` starts with one of the allow-list prefixes, or with one of the comma-separated extra prefixes (spaces around an entry are ignored,
// an empty entry matches nothing). A null or empty name is not allowed (the judge keeps an unreadable name by its own rule).
inline bool concourseAllowName(const char* name, const char* extraPrefixes) noexcept {
    if (!name || !*name) return false;
    for (const char* p : kAllowPrefixes)
        if (hasPrefixExact(name, p, std::strlen(p))) return true;
    if (!extraPrefixes) return false;
    const char* s = extraPrefixes;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') ++s;
        const char* e = s;
        while (*e && *e != ',') ++e;
        const char* t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t')) --t;
        if (t > s && hasPrefixExact(name, s, static_cast<size_t>(t - s))) return true;
        s = e;
    }
    return false;
}

// The gate: on foot and in social space by Status.json, the on-foot word known, and gameplay seen in this process. Gameplay is required: a leftover
// on-foot bit otherwise culls the main menu's hangar. `known` is journalOnFootKnown() or, in the flat profile (where that is hard false), gameplay.
inline bool concourseSocialOnFoot(uint32_t flags2, bool known, bool gameplay) noexcept {
    return gameplay && known && (flags2 & kFlags2OnFoot) != 0 && (flags2 & kFlags2SocialSpace) != 0;
}

// A record that is small and close is kept whatever its name: a weapon or a prop whose name is generic (Barrel, Slider, Hinge).
constexpr float kSmallRadiusM = 25.0f;
constexpr float kCloseDistanceM = 40.0f;

enum class Judge : uint8_t { kKeepName, kKeepUnreadableName, kKeepUnreadableSphere, kKeepSmallClose, kCull };

inline bool judgeKeeps(Judge j) noexcept { return j != Judge::kCull; }

// One record. Keep an unreadable name; keep an allowed name; keep an unreadable sphere (not finite, or no radius); keep a small, close record; cull.
inline Judge concourseJudge(bool nameReadable, bool allowed, bool sphereReadable, float distance, float radius) noexcept {
    if (!nameReadable) return Judge::kKeepUnreadableName;
    if (allowed) return Judge::kKeepName;
    if (!sphereReadable || !std::isfinite(distance) || !std::isfinite(radius) || !(radius > 0.0f)) return Judge::kKeepUnreadableSphere;
    if (radius <= kSmallRadiusM && distance <= kCloseDistanceM) return Judge::kKeepSmallClose;
    return Judge::kCull;
}

// A rig's records in one pass: how many were judged and how many of those were kept.
struct RigTally {
    uint32_t judged = 0;
    uint32_t kept = 0;
    void note(bool keep) noexcept {
        ++judged;
        if (keep) ++kept;
    }
};

// A rig is vetoed only if at least one of its records was judged in the pass and none was kept. A rig with nothing judged (never seen, or only
// records the judgement could not see) forwards.
inline bool concourseRigVetoed(uint32_t judged, uint32_t kept) noexcept { return judged > 0 && kept == 0; }
inline bool concourseRigVetoed(const RigTally& t) noexcept { return concourseRigVetoed(t.judged, t.kept); }

// ---- The glass panes: a draw named by its vertex shader hash, pixel shader hash and index count -----------------------------------------------
struct PaneTriple {
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t n = 0;
};

constexpr size_t kPaneListCap = 16;

inline bool paneTripleMatch(const PaneTriple* list, size_t count, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (list[i].n == n && list[i].vs == vs && list[i].ps == ps) return true;
    return false;
}

// The index of the triple in the list, or -1.
inline int paneTripleIndex(const PaneTriple* list, size_t count, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (list[i].n == n && list[i].vs == vs && list[i].ps == ps) return static_cast<int>(i);
    return -1;
}

// 16 hex digits to a value; false for any other length or a non-hex digit.
inline bool parseHex16(const char* s, size_t len, uint64_t* out) noexcept {
    if (len != 16) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

// One entry "VS/PS/N": two 16-digit hex hashes and a decimal index count (1 to 4294967295).
inline bool parsePaneEntry(const char* s, size_t len, PaneTriple* out) noexcept {
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) {
        ++s;
        --len;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) --len;
    if (len < 16 + 1 + 16 + 1 + 1 || s[16] != '/' || s[33] != '/') return false;
    PaneTriple t;
    if (!parseHex16(s, 16, &t.vs) || !parseHex16(s + 17, 16, &t.ps)) return false;
    uint64_t n = 0;
    for (size_t i = 34; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        n = n * 10 + static_cast<uint64_t>(s[i] - '0');
        if (n > 0xFFFFFFFFull) return false;
    }
    if (n == 0) return false;
    t.n = static_cast<uint32_t>(n);
    *out = t;
    return true;
}

// The list "VS/PS/N,VS/PS/N": the good entries in order, up to cap, duplicates dropped. *bad counts the entries that did not parse (an empty entry between
// commas is not counted). Returns the number kept.
inline size_t parsePaneList(const char* text, PaneTriple* out, size_t cap, size_t* bad) noexcept {
    size_t n = 0;
    if (bad) *bad = 0;
    if (!text) return 0;
    const char* s = text;
    while (*s) {
        const char* e = s;
        while (*e && *e != ',') ++e;
        const char* a = s;
        const char* b = e;
        while (a < b && (*a == ' ' || *a == '\t')) ++a;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) --b;
        if (b > a) {
            PaneTriple t;
            if (!parsePaneEntry(a, static_cast<size_t>(b - a), &t)) {
                if (bad) ++*bad;
            } else if (n < cap && !paneTripleMatch(out, n, t.vs, t.ps, t.n)) {
                out[n++] = t;
            }
        }
        s = *e ? e + 1 : e;
    }
    return n;
}

// The pane colours, fixed per slot, then repeating: red, green, blue, yellow, magenta, cyan, orange, white.
constexpr size_t kPaneColourCount = 8;
constexpr const char* kPaneColourNames[kPaneColourCount] = {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "white"};
constexpr float kPaneColours[kPaneColourCount][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f},
                                                     {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}};
constexpr float kPaneTintIntensity = 2.0f;

}  // namespace edvr::concourse
