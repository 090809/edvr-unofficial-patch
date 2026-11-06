#pragma once
// The concourse cull's pure half (docs\concourse-cull-2026-11-05.md): on foot in a large station's social space, the records that are not the room are
// not drawn. Nothing here touches Windows, the game or the log, so the optimized station's rig can test every rule. The engine half is in
// optimized_station.cpp (the judgement at the rig, the producer and the store) and concourse_pane.cpp (the panes).
//   concourseAllowName       the name allow-list: a prefix match, exact case, plus the temporary extra list (diag.concourse_keep)
//   concourseSocialOnFoot    the gate: Status.json Flags2 bit 0 (on foot) and bit 14 (social space), the file known, gameplay seen
//   concourseJudge           one record: keep (and why) or cull
//   concourseRigVetoed       one rig: vetoed only when a record of it was judged and none was kept
//   PaneTriple, paneTripleMatch, parsePaneList   a glass pane named by its vertex shader, pixel shader and index count (0: any count)
//   kPaneFamilyVs, kPaneFamilyPs, kPlumePs, kTwinPs, kBayIndexCount   the glass family, the draws withheld, the bay window's index count
//   fillPaneT8, fillPaneT9, fillPaneT10   the texels of the mirror materials
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
                                          "GUI_Interactive",     "Interactive_", "PlantStack", "TP_",   "HangarLobby", "TurboLift"};
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

// The glass family seen at the bay window (flight 1): one vertex shader and one pixel shader, any index count. The bay window is 36 indices; the other
// counts of the family (192, 864) are unidentified.
constexpr uint64_t kPaneFamilyVs = 0x98397963AAEC45D3ull;
constexpr uint64_t kPaneFamilyPs = 0x66B08F89E4926C01ull;
constexpr uint32_t kBayIndexCount = 36;
// The draws that come after the panes and paint over them: the plume and its 1024x1024 twin, named by their pixel shaders (EDVR swaps the plume's vertex
// shader at draw time, so the vertex shader is not a stable name).
constexpr uint64_t kPlumePs = 0x3B0B38CD96F53BC1ull;
constexpr uint64_t kTwinPs = 0x34A1475B2C790DFAull;

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
// base colour, a = roughness. Each texture is kPaneTexSize square; a tile seam one texel wide runs every kPaneSeamPeriod texels.
constexpr uint32_t kPaneTexSize = 64;
constexpr uint32_t kPaneSeamPeriod = 16;

inline uint32_t paneHash(uint32_t x, uint32_t y, uint32_t salt) noexcept {
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ salt * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
inline bool paneSeam(uint32_t x, uint32_t y) noexcept { return x % kPaneSeamPeriod == 0 || y % kPaneSeamPeriod == 0; }
// A deterministic offset in [-amp, amp].
inline int paneNoise(uint32_t x, uint32_t y, uint32_t salt, int amp) noexcept {
    return static_cast<int>(paneHash(x, y, salt) % static_cast<uint32_t>(2 * amp + 1)) - amp;
}
inline uint8_t paneClampByte(int v) noexcept { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// out: kPaneTexSize * kPaneTexSize * 2 bytes. Metalness 255 and the second channel 255 everywhere.
inline void fillPaneT8(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize; ++i) {
        out[2 * i] = 255;
        out[2 * i + 1] = 255;
    }
}
// Flat 127, a seam of about +-24 (the sign alternates by seam line), noise of +-3. out: kPaneTexSize * kPaneTexSize * 2 bytes.
inline void fillPaneT9(uint8_t* out) noexcept {
    for (uint32_t y = 0; y < kPaneTexSize; ++y)
        for (uint32_t x = 0; x < kPaneTexSize; ++x) {
            int vx = 127 + paneNoise(x, y, 1, 3);
            int vy = 127 + paneNoise(x, y, 2, 3);
            if (x % kPaneSeamPeriod == 0) vx += (x / kPaneSeamPeriod) % 2 ? 24 : -24;
            if (y % kPaneSeamPeriod == 0) vy += (y / kPaneSeamPeriod) % 2 ? 24 : -24;
            uint8_t* o = out + 2 * (y * kPaneTexSize + x);
            o[0] = paneClampByte(vx);
            o[1] = paneClampByte(vy);
        }
}
// rgb about (0.72, 0.75, 0.78) with about 4% variation, a = roughness about 0.18; seams darker and rougher. out: kPaneTexSize * kPaneTexSize * 4 bytes.
inline void fillPaneT10(uint8_t* out) noexcept {
    constexpr int kBase[3] = {184, 191, 199};   // 0.72, 0.75, 0.78 of 255
    for (uint32_t y = 0; y < kPaneTexSize; ++y)
        for (uint32_t x = 0; x < kPaneTexSize; ++x) {
            const bool seam = paneSeam(x, y);
            uint8_t* o = out + 4 * (y * kPaneTexSize + x);
            const int var = paneNoise(x, y, 3, 7);   // +-7 of about 190 is +-3.7%
            for (int c = 0; c < 3; ++c) o[c] = paneClampByte(kBase[c] + var - (seam ? 40 : 0));
            o[3] = paneClampByte(46 + paneNoise(x, y, 4, 3) + (seam ? 50 : 0));   // 46 / 255 = 0.18
        }
}

// ---- The bay glass's clearing on approach --------------------------------------------------------------------------------------------------------
// Mirror (the glass stands in, the cull is on) -> Lifting (the cull is off, the glass still opaque, so the station is back before it opens) -> FadingOut ->
// Clear -> (the camera is exitMetres from where the glass was left) FadingIn -> Mirror. A reading of the bay glass's share of the view at or over enterShare,
// twice in a row in Mirror, starts it; in Clear it holds the latch to the camera; in FadingIn it turns the fade round.
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

constexpr double kClearExitMetres = 10.0;
constexpr uint64_t kClearLiftMs = 150;
constexpr uint64_t kClearFadeMs = 600;
constexpr uint32_t kClearReadingsToEnter = 2;

struct ClearInput {
    uint64_t nowMs = 0;
    bool gate = false;        // the cull is wanted: key on, on foot in social space, the anchor seen, not sealed
    float bayShare = -1.0f;   // the latest completed frame's share of the view (0 to 1); negative when there is no reading
    uint32_t shareSeq = 0;    // moves with each new reading: one frame read on two ticks is one reading
    bool cameraValid = false;
    float cam[3] = {};
    float enterShare = 0.0f;   // 0 never clears
    double exitMetres = kClearExitMetres;
    uint64_t liftMs = kClearLiftMs;
    uint64_t fadeMs = kClearFadeMs;
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
        switch (phase_) {
            case ClearPhase::kMirror:
                opacity_ = 1.0f;
                if (fresh) hits_ = over ? hits_ + 1 : 0;
                if (in.enterShare > 0.0f && hits_ >= kClearReadingsToEnter && in.cameraValid) {
                    latch(in);
                    enter(ClearPhase::kLifting, in.nowMs);
                }
                break;
            case ClearPhase::kLifting:
                opacity_ = 1.0f;
                if (in.nowMs - startMs_ >= in.liftMs) {
                    fromOpacity_ = 1.0f;
                    enter(ClearPhase::kFadingOut, in.nowMs);
                }
                break;
            case ClearPhase::kFadingOut:
                opacity_ = fromOpacity_ - fadeFraction(in);
                if (opacity_ <= 0.0f) {
                    opacity_ = 0.0f;
                    enter(ClearPhase::kClear, in.nowMs);
                }
                break;
            case ClearPhase::kClear:
                opacity_ = 0.0f;
                if (over && in.cameraValid) {
                    latch(in);
                } else if (in.cameraValid && distanceFromLatch(in) >= in.exitMetres) {
                    fromOpacity_ = 0.0f;
                    enter(ClearPhase::kFadingIn, in.nowMs);
                }
                break;
            case ClearPhase::kFadingIn:
                opacity_ = fromOpacity_ + fadeFraction(in);
                if (opacity_ > 1.0f) opacity_ = 1.0f;
                if (over) {   // turned round: fading out from where the fade had got to, and the latch is where the view is
                    fromOpacity_ = opacity_;
                    if (in.cameraValid) latch(in);
                    enter(ClearPhase::kFadingOut, in.nowMs);
                    break;
                }
                if (opacity_ >= 1.0f) {
                    opacity_ = 1.0f;
                    hits_ = 0;
                    enter(ClearPhase::kMirror, in.nowMs);
                }
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
    }
    void latch(const ClearInput& in) noexcept {
        for (int k = 0; k < 3; ++k) latch_[k] = in.cam[k];
    }
    double distanceFromLatch(const ClearInput& in) const noexcept {
        double s = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double d = static_cast<double>(in.cam[k]) - static_cast<double>(latch_[k]);
            s += d * d;
        }
        return std::sqrt(s);
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
    float latch_[3] = {};
};

}  // namespace edvr::concourse
