#pragma once
// The concourse cull's pure half (docs\concourse-cull-2026-11-05.md): on foot in a large station's social space, the records that are not the room are
// not drawn. Nothing here touches Windows, the game or the log, so the optimized station's rig can test every rule. The engine half is in
// optimized_station.cpp (the judgement at the rig, the producer and the store) and concourse_pane.cpp (the panes).
//   concourseAllowName       the name allow-list: a prefix match, exact case, plus the temporary extra list (diag.concourse_keep)
//   concourseSocialOnFoot    the gate: Status.json Flags2 bit 0 (on foot) and bit 14 (social space), the file known, gameplay seen
//   concourseJudge           one record: keep (and why) or cull
//   concourseRigVetoed       one rig: vetoed only when a record of it was judged and none was kept
//   PaneTriple, paneTripleMatch, parsePaneList   a glass pane named by its vertex shader, pixel shader and index count (0: any count)
//   kPaneFamilyVs, kPaneFamilyPs, kPlumePs, kTwinPs, kBayIndexCount   the glass family, the plume pair, the bay window's index count
//   kWithheldPs, kWithheldVs, withheldPsIndex, withheldVsIndex   the outside draws not issued while the cull is on, named by shader
//   fillPaneT8, fillPaneT9, fillPaneT10   the texels of the mirror materials (uniform)
//   markDepthForDistance, PaneSides, paneTestThisFrame   the mirror's mark depth, and each pane's side (interior or exterior) learned from two occlusion counts
//   ClearMachine   the bay glass's clearing on approach: a pure state machine
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
constexpr const char* kAllowPrefixes[] = {"Standard_SocialArea", "SA_",         "PS_",        "STAT_", "FUI_",        "PROP_",
                                          "GUI_Interactive",     "Interactive_", "TP_",         "HangarLobby", "TurboLift"};
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
// An allowed name whose sphere is wholly farther than this from the camera is culled all the same (the plant stacks, the far hangar lobby, the huge logo);
// the anchor and the hub are exempt, because the gate reads them from the store.
constexpr float kAllowedFarM = 200.0f;

enum class Judge : uint8_t { kKeepName, kKeepUnreadableName, kKeepUnreadableSphere, kKeepSmallClose, kCull };

inline bool judgeKeeps(Judge j) noexcept { return j != Judge::kCull; }

// One record. Keep an unreadable name; an allowed name is kept, unless its sphere is readable and wholly past kAllowedFarM (distance - radius) and it is not
// the anchor or the hub (`farExempt`, computed from the name by the caller); keep an unreadable sphere (not finite, or no radius); keep a small, close record; cull.
inline Judge concourseJudge(bool nameReadable, bool allowed, bool sphereReadable, float distance, float radius, bool farExempt) noexcept {
    if (!nameReadable) return Judge::kKeepUnreadableName;
    const bool sphereOk = sphereReadable && std::isfinite(distance) && std::isfinite(radius) && radius > 0.0f;
    if (allowed) return (!farExempt && sphereOk && distance - radius > kAllowedFarM) ? Judge::kCull : Judge::kKeepName;
    if (!sphereOk) return Judge::kKeepUnreadableSphere;
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

// The glass family seen at the bay window (flight 1): one vertex shader and one pixel shader, any index count. The bay window is 36 indices; the other
// counts of the family (192, 864) are unidentified.
constexpr uint64_t kPaneFamilyVs = 0x98397963AAEC45D3ull;
constexpr uint64_t kPaneFamilyPs = 0x66B08F89E4926C01ull;
constexpr uint32_t kBayIndexCount = 36;
// The draws that come after the panes and paint over them: the plume and its 1024x1024 twin, named by their pixel shaders (EDVR swaps the plume's vertex
// shader at draw time, so the vertex shader is not a stable name).
constexpr uint64_t kPlumePs = 0x3B0B38CD96F53BC1ull;
constexpr uint64_t kTwinPs = 0x34A1475B2C790DFAull;

// ---- The draws withheld while the cull is on: what is outside the room (smoke, volumetrics, ship lasers, engine trails, the planet) --------------------
// Named by pixel shader hash or vertex shader hash (a draw with either is not issued). A terrain patch has no pixel shader, so it is named by its vertex
// shader. Not here, on purpose: the player's first-person laser (0x88DCF1164C640EC3 / 494506A6), the room's glows (1C506222, 359BF8FF), the sky (F8FA801F),
// the stars (68DDDEF0, F7A6E916), the dust (E904D334). Beams, flares and the scatter are non-indexed draws, so the plain draw thunks carry the scope too.
struct WithheldFamily {
    uint64_t hash;
    const char* name;
};
constexpr WithheldFamily kWithheldPs[] = {
    {0x3B0B38CD96F53BC1ull, "plume"},
    {0x34A1475B2C790DFAull, "plume twin"},
    {0x42AC0CACC9CDF72Bull, "trail ribbon"},
    {0xEEAAC839A9F09448ull, "trail volume"},
    {0x6FD4C38BA927C8C7ull, "trail haze"},
    {0xA75C1DB6562B8CA7ull, "beams and bolts"},
    {0xC5DF9CC943476289ull, "multicannon rounds"},
    {0xC49F999F7D3C801Dull, "shield strike"},
    {0xBD801F2FB02522EBull, "smoke"},
    {0x6EF82262EB12A037ull, "atmosphere"},
    {0x5BCB6B95BE7C0700ull, "atmosphere quad A"},
    {0x97DBC87FCAA429C4ull, "atmosphere quad B"},
    {0xD56F859BE4781431ull, "flares"},
    {0x43E5E6EB67AC751Bull, "planet surface"},
    {0x41152F82C6E8BE1Full, "scatter"},               // VS AFFEF018: fullscreen, additive, no depth test, reads only the depth
    {0x2BAE3742FEB916D9ull, "particles A"},           // VS AFBCD3AD, 9F4BBCFC (docs\billboard-root-cause-2026-10-16.md)
    {0x3789CA2062E196FBull, "particles B"},           // VS 9AEC596A
    {0xE54F2A902E5631F6ull, "particles C"},           // VS BBAD1CA8
    {0x6EAA86EFE135B2D4ull, "shield strike part 2"},  // VS 820E5C13, the same 3984 indices as the shield strike
};
constexpr WithheldFamily kWithheldVs[] = {
    {0x72BDD292154158ADull, "body surface"},
    {0x26FC402B1274EE7Bull, "planetary pipeline"},
    {0x9FFA5D5E79F04873ull, "planetary 2"},
    {0x3530A6FD15EDE145ull, "planetary 3"},
    {0x71DD9863DCFC0986ull, "planet surface vs"},
    {0xACE405F428C17EF6ull, "terrain patch"},
    {0x19F70CE80DA3242Bull, "body sphere"},
    {0xC7FA0C0F5DD49180ull, "beside the bodies"},
    {0x8BD7C37ABCEE7E45ull, "sun"},
    {0x1F3AD1584D7FA3C8ull, "sun arcs"},
};
constexpr size_t kWithheldPsCount = sizeof(kWithheldPs) / sizeof(kWithheldPs[0]);
constexpr size_t kWithheldVsCount = sizeof(kWithheldVs) / sizeof(kWithheldVs[0]);

// A 64-bit bloom on (hash & 63): a draw whose bit is clear is in no list, with no scan.
constexpr uint64_t withheldBloom(const WithheldFamily* list, size_t n) noexcept {
    uint64_t b = 0;
    for (size_t i = 0; i < n; ++i) b |= uint64_t(1) << (list[i].hash & 63u);
    return b;
}
constexpr uint64_t kWithheldPsBloom = withheldBloom(kWithheldPs, kWithheldPsCount);
constexpr uint64_t kWithheldVsBloom = withheldBloom(kWithheldVs, kWithheldVsCount);

// The index of `hash` in the list, or -1. A zero hash (no shader) is never in a list.
inline int withheldIndex(const WithheldFamily* list, size_t n, uint64_t bloom, uint64_t hash) noexcept {
    if (hash == 0 || (bloom & (uint64_t(1) << (hash & 63u))) == 0) return -1;
    for (size_t i = 0; i < n; ++i)
        if (list[i].hash == hash) return static_cast<int>(i);
    return -1;
}
inline int withheldPsIndex(uint64_t hash) noexcept { return withheldIndex(kWithheldPs, kWithheldPsCount, kWithheldPsBloom, hash); }
inline int withheldVsIndex(uint64_t hash) noexcept { return withheldIndex(kWithheldVs, kWithheldVsCount, kWithheldVsBloom, hash); }

// The light cones (VS 1C506222, n=384) and their glow quads (VS 359BF8FF, n=6): the green haze and the pale wedges past the glass, up to 40% of the screen in Flight 1's
// capture. Nothing yet says none of them is a room lamp's, so while the cull is on they are not withheld: they are limited by the mirror's stencil mark, so a pixel
// with nothing near behind it is dropped and one over the room stays.
inline bool farLimitedPs(uint64_t ps) noexcept { return ps == 0x2519C9050946D545ull || ps == 0x92FF8499ED345759ull; }

// True when a list holds the same hash twice (the tests fail on it).
constexpr bool withheldHasDuplicate(const WithheldFamily* list, size_t n) noexcept {
    for (size_t i = 0; i < n; ++i)
        for (size_t k = i + 1; k < n; ++k)
            if (list[i].hash == list[k].hash) return true;
    return false;
}

// An entry whose n is 0 matches any index count.
inline bool paneEntryMatches(const PaneTriple& e, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    return e.vs == vs && e.ps == ps && (e.n == 0 || e.n == n);
}

inline bool paneTripleMatch(const PaneTriple* list, size_t count, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (paneEntryMatches(list[i], vs, ps, n)) return true;
    return false;
}

// The index of the first matching triple in the list, or -1.
inline int paneTripleIndex(const PaneTriple* list, size_t count, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (paneEntryMatches(list[i], vs, ps, n)) return static_cast<int>(i);
    return -1;
}

// True when an entry with exactly this vs, ps and n is in the list (the parser's duplicate test: 0 and 36 are different entries).
inline bool paneTripleHas(const PaneTriple* list, size_t count, const PaneTriple& t) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (list[i].vs == t.vs && list[i].ps == t.ps && list[i].n == t.n) return true;
    return false;
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

// One entry "VS/PS" (any index count, n = 0) or "VS/PS/N": two 16-digit hex hashes and a decimal index count (1 to 4294967295).
inline bool parsePaneEntry(const char* s, size_t len, PaneTriple* out) noexcept {
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) {
        ++s;
        --len;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) --len;
    if (len < 16 + 1 + 16 || s[16] != '/') return false;
    PaneTriple t;
    if (!parseHex16(s, 16, &t.vs) || !parseHex16(s + 17, 16, &t.ps)) return false;
    if (len == 16 + 1 + 16) {
        t.n = 0;
        *out = t;
        return true;
    }
    if (len < 16 + 1 + 16 + 1 + 1 || s[33] != '/') return false;
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
            } else if (n < cap && !paneTripleHas(out, n, t)) {
                out[n++] = t;
            }
        }
        s = *e ? e + 1 : e;
    }
    return n;
}

// The pane colours, fixed per distinct index count in the order the counts are first seen, then repeating: red, green, blue, yellow, magenta, cyan, orange, white.
constexpr size_t kPaneColourCount = 8;
constexpr const char* kPaneColourNames[kPaneColourCount] = {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "white"};
constexpr float kPaneColours[kPaneColourCount][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f},
                                                     {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}};
constexpr float kPaneTintIntensity = 2.0f;

// ---- The mirror materials (the game's glass shader reads three texture arrays; these stand in for them) ------------------------------------------
// t8 (R8G8) x = metalness, y = the game's o0.w; t9 (R8G8) the normal's xy, decoded by the shader as xy * 2.007874 - 0.999985 (127 is flat); t10 (R8G8B8A8) rgb =
// base colour, a = roughness. Each texture is kPaneTexSize square and uniform: no seam, no noise, so the mirror is smooth.
constexpr uint32_t kPaneTexSize = 4;
constexpr uint8_t kPaneT10Rgb[3] = {184, 191, 199};   // 0.72, 0.75, 0.78 of 255
constexpr uint8_t kPaneT10Roughness = 77;             // 0.30 of 255

// out: kPaneTexSize * kPaneTexSize * 2 bytes. Metalness 255 and the second channel 255 everywhere.
inline void fillPaneT8(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize; ++i) {
        out[2 * i] = 255;
        out[2 * i + 1] = 255;
    }
}
// Flat 127, 127. out: kPaneTexSize * kPaneTexSize * 2 bytes.
inline void fillPaneT9(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize; ++i) {
        out[2 * i] = 127;
        out[2 * i + 1] = 127;
    }
}
// One colour, roughness 0.30. out: kPaneTexSize * kPaneTexSize * 4 bytes.
inline void fillPaneT10(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize; ++i) {
        for (int c = 0; c < 3; ++c) out[4 * i + c] = kPaneT10Rgb[c];
        out[4 * i + 3] = kPaneT10Roughness;
    }
}

// ---- The mirror's mark: where the scene behind a pane is farther than the beyond distance -----------------------------------------------------------
// Scene depth is infinite reversed Z (near plane 0.025 m): ndc z at distance d is 0.025 / d.
constexpr float kMirrorNearM = 0.025f;
constexpr float kMirrorBeyondDefaultM = 100.0f;
constexpr float kMirrorBeyondMinM = 20.0f;
constexpr float kMirrorBeyondMaxM = 5000.0f;
inline float markDepthForDistance(float metres) noexcept {
    if (!(metres >= 1.0f)) metres = 1.0f;
    return kMirrorNearM / metres;
}

// ---- Each pane's side, learned by occlusion queries ----------------------------------------------------------------------------------------------
// A pane is the game's glass draw, named by its index count, first index and base vertex. Interior glass has a wall behind it; exterior glass has nothing near
// behind it. One test reads two queries: the samples the game's glass passed (drawn) and the samples the marked mirror draw passed (mirrored).
struct PaneKey {
    uint32_t n = 0, startIndex = 0;
    int32_t baseVertex = 0;
};
enum class PaneSide : uint8_t { kUnknown, kInterior, kExterior };

constexpr uint64_t kPaneTestMinSamples = 64;
constexpr double kPaneExteriorRatio = 0.6;
constexpr uint32_t kPaneTestPeriod = 8;
constexpr size_t kPaneSideCap = 256;

inline uint32_t paneHash(uint32_t x, uint32_t y, uint32_t salt) noexcept {
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ salt * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
inline uint32_t paneKeyHash(const PaneKey& k) noexcept { return paneHash(k.n, k.startIndex, static_cast<uint32_t>(k.baseVertex) ^ 0x51ED270Bu); }

// Whether a key is tested this frame: one frame in eight per key, spread by key.
inline bool paneTestThisFrame(const PaneKey& k, uint32_t frame) noexcept { return (paneKeyHash(k) + frame) % kPaneTestPeriod == 0; }

class PaneSides {
public:
    PaneSide side(const PaneKey& k) const noexcept {
        const int i = find(k);
        return i < 0 ? PaneSide::kUnknown : sides_[i];
    }
    // The result of one note: whether the key's side is new or changed, for the log.
    enum class Change : uint8_t { kNone, kFirst, kChanged };
    Change note(const PaneKey& k, uint64_t drawn, uint64_t mirrored) noexcept {
        if (drawn < kPaneTestMinSamples) return Change::kNone;
        const PaneSide now = static_cast<double>(mirrored) >= kPaneExteriorRatio * static_cast<double>(drawn) ? PaneSide::kExterior : PaneSide::kInterior;
        int i = find(k);
        if (i >= 0) {
            // One mesh drawn at two places gives two readings of very different size: only a reading at least half the largest seen can change the side.
            if (drawn > maxDrawn_[i]) maxDrawn_[i] = drawn;
            if (drawn * 2 < maxDrawn_[i]) return Change::kNone;
        }
        if (i < 0) {
            if (count_ >= kPaneSideCap) {
                ++dropped_;
                return Change::kNone;
            }
            i = static_cast<int>(count_++);
            keys_[i] = k;
            sides_[i] = now;
            maxDrawn_[i] = drawn;
            if (now == PaneSide::kExterior) ++exterior_;
            return Change::kFirst;
        }
        if (sides_[i] == now) return Change::kNone;
        if (sides_[i] == PaneSide::kExterior) --exterior_;
        if (now == PaneSide::kExterior) ++exterior_;
        sides_[i] = now;
        return Change::kChanged;
    }
    void reset() noexcept {
        count_ = 0;
        exterior_ = 0;
        dropped_ = 0;
        for (auto& m : maxDrawn_) m = 0;
    }
    uint32_t exteriorCount() const noexcept { return exterior_; }
    uint32_t knownCount() const noexcept { return count_; }
    uint32_t dropped() const noexcept { return dropped_; }

private:
    int find(const PaneKey& k) const noexcept {
        for (uint32_t i = 0; i < count_; ++i)
            if (keys_[i].n == k.n && keys_[i].startIndex == k.startIndex && keys_[i].baseVertex == k.baseVertex) return static_cast<int>(i);
        return -1;
    }
    PaneKey keys_[kPaneSideCap];
    PaneSide sides_[kPaneSideCap] = {};
    uint64_t maxDrawn_[kPaneSideCap] = {};
    uint32_t count_ = 0, exterior_ = 0, dropped_ = 0;
};

// ---- The bay glass's clearing, following the bay in view ------------------------------------------------------------------------------------------
// Mirror (the glass stands in, the cull is on) -> Lifting (the cull is off, the glass still opaque, so the station is back before it opens) -> FadingOut ->
// Clear. A reading of the bay glass's share of the view at or over enterShare, twice in a row in Mirror, starts it. In Lifting, FadingOut and Clear the share
// below enterShare * kClearExitRatio for releaseMs running releases it: with the bay out of view (share at most kClearUnseenShare, or no reading) straight back
// to Mirror, else FadingIn, which returns to Mirror when it is done, turns round to FadingOut if the share is over enterShare again, and jumps to Mirror
// if the bay goes out of view.
enum class ClearPhase : uint8_t { kMirror, kLifting, kFadingOut, kClear, kFadingIn };

inline const char* clearPhaseName(ClearPhase p) noexcept {
    switch (p) {
        case ClearPhase::kMirror: return "mirror";
        case ClearPhase::kLifting: return "lifting";
        case ClearPhase::kFadingOut: return "fading-out";
        case ClearPhase::kClear: return "clear";
        default: return "fading-in";
    }
}

constexpr float kClearExitRatio = 0.4f;     // the exit share is enterShare * this (20% in, 8% out)
constexpr uint64_t kClearReleaseMs = 250;   // the share must stay below the exit share this long
constexpr float kClearUnseenShare = 0.01f;  // at or under this (or no reading) the bay is out of view
constexpr uint64_t kClearLiftMs = 150;
constexpr uint64_t kClearFadeMs = 600;
constexpr uint32_t kClearReadingsToEnter = 2;

struct ClearInput {
    uint64_t nowMs = 0;
    bool gate = false;        // the cull is wanted: key on, on foot in social space, the anchor seen, not sealed
    float bayShare = -1.0f;   // the latest completed frame's share of the view (0 to 1); negative when there is no reading
    uint32_t shareSeq = 0;    // moves with each new reading: one frame read on two ticks is one reading
    float enterShare = 0.0f;   // 0 never clears
    uint64_t liftMs = kClearLiftMs;
    uint64_t fadeMs = kClearFadeMs;
    uint64_t releaseMs = kClearReleaseMs;
};
struct ClearOutput {
    bool cullOn = true;
    float bayOpacity = 1.0f;
    ClearPhase phase = ClearPhase::kMirror;
};

class ClearMachine {
public:
    ClearPhase phase() const noexcept { return phase_; }
    float opacity() const noexcept { return opacity_; }
    void reset() noexcept {
        phase_ = ClearPhase::kMirror;
        opacity_ = 1.0f;
        hits_ = 0;
        seqSeen_ = false;
        holding_ = false;
    }
    ClearOutput step(const ClearInput& in) noexcept {
        if (!in.gate) {
            reset();
            return out();
        }
        const bool fresh = !seqSeen_ || in.shareSeq != lastSeq_;
        seqSeen_ = true;
        lastSeq_ = in.shareSeq;
        const bool over = in.enterShare > 0.0f && in.bayShare >= 0.0f && in.bayShare >= in.enterShare;
        const bool below = in.enterShare <= 0.0f || in.bayShare < in.enterShare * kClearExitRatio;   // a negative share (no reading) is below
        const bool unseen = in.bayShare <= kClearUnseenShare;                                         // negative included
        switch (phase_) {
            case ClearPhase::kMirror:
                opacity_ = 1.0f;
                if (fresh) hits_ = over ? hits_ + 1 : 0;
                if (in.enterShare > 0.0f && hits_ >= kClearReadingsToEnter) enter(ClearPhase::kLifting, in.nowMs);
                break;
            case ClearPhase::kLifting:
            case ClearPhase::kFadingOut:
            case ClearPhase::kClear:
                if (released(below, in)) {
                    if (unseen || phase_ == ClearPhase::kLifting) {
                        toMirror(in.nowMs);
                    } else {
                        fromOpacity_ = opacity_;
                        enter(ClearPhase::kFadingIn, in.nowMs);
                    }
                    break;
                }
                if (phase_ == ClearPhase::kLifting) {
                    opacity_ = 1.0f;
                    if (in.nowMs - startMs_ >= in.liftMs) {
                        fromOpacity_ = 1.0f;
                        enter(ClearPhase::kFadingOut, in.nowMs);
                    }
                } else if (phase_ == ClearPhase::kFadingOut) {
                    opacity_ = fromOpacity_ - fadeFraction(in);
                    if (opacity_ <= 0.0f) {
                        opacity_ = 0.0f;
                        enter(ClearPhase::kClear, in.nowMs);
                    }
                } else {
                    opacity_ = 0.0f;
                }
                break;
            case ClearPhase::kFadingIn:
                opacity_ = fromOpacity_ + fadeFraction(in);
                if (opacity_ > 1.0f) opacity_ = 1.0f;
                if (over) {   // turned round: fading out from where the fade had got to
                    fromOpacity_ = opacity_;
                    enter(ClearPhase::kFadingOut, in.nowMs);
                    break;
                }
                if (unseen || opacity_ >= 1.0f) toMirror(in.nowMs);   // done, or the bay went out of view: the cull returns now
                break;
        }
        return out();
    }

private:
    ClearOutput out() const noexcept {
        ClearOutput o;
        o.cullOn = phase_ == ClearPhase::kMirror;
        o.bayOpacity = opacity_;
        o.phase = phase_;
        return o;
    }
    void enter(ClearPhase p, uint64_t now) noexcept {
        phase_ = p;
        startMs_ = now;
        if (p != ClearPhase::kFadingOut && p != ClearPhase::kClear) holding_ = false;   // a hold carries on from Lifting to FadingOut to Clear
    }
    void toMirror(uint64_t now) noexcept {
        opacity_ = 1.0f;
        hits_ = 0;
        enter(ClearPhase::kMirror, now);
    }
    // The release hold: `below` held continuously for releaseMs. A reading at or over the exit share cancels it.
    bool released(bool below, const ClearInput& in) noexcept {
        if (!below) {
            holding_ = false;
            return false;
        }
        if (!holding_) {
            holding_ = true;
            holdStart_ = in.nowMs;
        }
        return in.nowMs - holdStart_ >= in.releaseMs;
    }
    // The elapsed part of one full fade: the rate is one unit of opacity per fadeMs, so a fade turned round part-way takes only the time it needs.
    float fadeFraction(const ClearInput& in) const noexcept {
        if (in.fadeMs == 0) return 1.0f;
        return static_cast<float>(static_cast<double>(in.nowMs - startMs_) / static_cast<double>(in.fadeMs));
    }

    ClearPhase phase_ = ClearPhase::kMirror;
    float opacity_ = 1.0f;
    float fromOpacity_ = 1.0f;
    uint32_t hits_ = 0;
    uint32_t lastSeq_ = 0;
    bool seqSeen_ = false;
    uint64_t startMs_ = 0;
    bool holding_ = false;
    uint64_t holdStart_ = 0;
};

}  // namespace edvr::concourse
