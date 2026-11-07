// The type of station the commander is at, read from the journal as its bytes stream past. Pure (no game, no Windows), so tools\optimized_station_test runs it directly;
// journal_watch.cpp feeds it every byte the tail reads, once, on the worker thread. The concourse cull's bake is chosen by it (concourse_cull_core.h, bakePlanFor).
//
// WHAT THE JOURNAL SAYS (3,097 journals, 2026-11-07). Docked, Embark, Disembark and a docked Location name the station: "StationName", "StationType", "MarketID". A Location written
// when the game loads on foot never does (580 of 580): it has only "Body":"<the station's name>" and "BodyType":"Station". So the last station named is saved, and a load on foot
// matches its Body against that. On a carrier Body is the star or planet, so a load on foot there reads unknown. Unknown is "not confirmed": the game's own station.
// A Stronghold Carrier is "StationType":"SurfaceStation", told apart only by its name. "Bernal" is the journal's other name for an Ocellus: Location has written it where Docked says
// Ocellus (INFERRED, from the two lists; no one station compared).
// AT A PORT ON A PLANET it goes differently. Only Docked and Undocked name it. Disembark and Embark there have "OnPlanet":true and no station (579 and 574 such lines, settlements among them), and so does a Location
// on foot, with "BodyType":"Planet" (399). So the type holds through those until Undocked, and a load on foot on a planet takes the saved type if that is a port on a planet. That also says so at
// a settlement or on open ground after the last port: what uses the type must have something else to go on, as the concourse cull has its anchor.
//
// PRIVACY. What is kept is the type, and a hash of the station's name so that the name itself is neither saved nor logged. A line is held only until its newline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace edvr {
namespace journal_station {

enum class Kind : uint8_t { kUnknown = 0, kCoriolis, kOrbis, kOcellus, kOutpost, kCraterPort, kCraterOutpost, kSurfaceStation, kAsteroidBase, kFleetCarrier, kMegaShip, kOnFootSettlement, kOther };

inline const char* kindName(Kind k) noexcept {
    switch (k) {
        case Kind::kCoriolis: return "Coriolis";
        case Kind::kOrbis: return "Orbis";
        case Kind::kOcellus: return "Ocellus";
        case Kind::kOutpost: return "Outpost";
        case Kind::kCraterPort: return "CraterPort";
        case Kind::kCraterOutpost: return "CraterOutpost";
        case Kind::kSurfaceStation: return "SurfaceStation";
        case Kind::kAsteroidBase: return "AsteroidBase";
        case Kind::kFleetCarrier: return "FleetCarrier";
        case Kind::kMegaShip: return "MegaShip";
        case Kind::kOnFootSettlement: return "OnFootSettlement";
        case Kind::kOther: return "another type";
        default: return "unknown";
    }
}

inline bool sameText(const char* p, size_t n, const char* text) noexcept {
    const size_t m = std::strlen(text);
    return n == m && std::memcmp(p, text, n) == 0;
}

// A type the journal names but this list does not is kOther, which no row runs.
inline Kind kindOf(const char* p, size_t n) noexcept {
    if (!p || n == 0) return Kind::kUnknown;
    if (sameText(p, n, "Bernal")) return Kind::kOcellus;
    for (uint8_t k = static_cast<uint8_t>(Kind::kCoriolis); k < static_cast<uint8_t>(Kind::kOther); ++k)
        if (sameText(p, n, kindName(static_cast<Kind>(k)))) return static_cast<Kind>(k);
    return Kind::kOther;
}

// FNV-1a. 0 is "nothing saved", so a name never hashes to it.
inline uint64_t hashName(const char* p, size_t n) noexcept {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(p[i]);
        h *= 0x100000001B3ull;
    }
    return h != 0 ? h : 1;
}

// Whether the commander is at a port on a planet's surface. The type says it where the journal has given one. A Disembark on a planet names no station, and neither does a load on foot, so
// with no type Status.json says: Flags bit 21 (a latitude and longitude: near a body) and Flags2 bit 4 (on foot on a planet), as a docked, on-foot commander at a CraterOutpost shows
// (0x00200005, 0x00014011; at an Orbis 0x00000005, 0x00014009). A type comes first because a station in a low orbit may have a latitude too.
inline bool surfaceKind(Kind k) noexcept { return k == Kind::kCraterPort || k == Kind::kCraterOutpost || k == Kind::kSurfaceStation || k == Kind::kOnFootSettlement; }
inline bool atSurfacePort(Kind k, uint32_t flags, uint32_t flags2) noexcept {
    if (surfaceKind(k)) return true;
    switch (k) {
        case Kind::kCoriolis:
        case Kind::kOrbis:
        case Kind::kOcellus:
        case Kind::kOutpost:
        case Kind::kAsteroidBase:
        case Kind::kFleetCarrier:
        case Kind::kMegaShip:
            return false;
        default:
            return (flags & (1u << 21)) != 0 || (flags2 & (1u << 4)) != 0;
    }
}

struct Saved {
    uint64_t nameHash = 0;
    Kind kind = Kind::kUnknown;
};

// The saved file's one line. Anything else reads as nothing saved.
inline constexpr char kSavedPrefix[] = "edvr-station 1 ";
inline int formatSaved(const Saved& s, char* out, size_t cap) noexcept {
    return std::snprintf(out, cap, "%s%016llx %s\n", kSavedPrefix, static_cast<unsigned long long>(s.nameHash), kindName(s.kind));
}
inline bool parseSaved(const char* text, Saved* out) noexcept {
    if (!text || !out) return false;
    const size_t prefix = sizeof(kSavedPrefix) - 1;
    if (std::strncmp(text, kSavedPrefix, prefix) != 0) return false;
    char* end = nullptr;
    const unsigned long long h = std::strtoull(text + prefix, &end, 16);
    if (!end || end != text + prefix + 16 || *end != ' ' || h == 0) return false;
    const char* name = end + 1;
    size_t n = 0;
    while (name[n] != '\0' && name[n] != '\r' && name[n] != '\n') ++n;
    const Kind k = kindOf(name, n);
    if (k == Kind::kUnknown || k == Kind::kOther) return false;
    out->nameHash = h;
    out->kind = k;
    return true;
}

class Latch {
public:
    static constexpr uint32_t kLineCap = 4096;   // Location names its Body well inside this; its faction lists, past it, are not read

    Kind kind() const noexcept { return kind_; }
    const Saved& saved() const noexcept { return saved_; }
    void restore(const Saved& s) noexcept { saved_ = s; }
    // True once after each change of what should be saved.
    bool takeSavedChanged() noexcept {
        const bool was = savedChanged_;
        savedChanged_ = false;
        return was;
    }
    // Another file is another stream of bytes. What it means for the station does not change: the events say that, and a part of a long session starts a new file with no event.
    void newFile() noexcept { len_ = 0; }
    // True once after a load on foot that did not match what was saved, with the hash of where it says it is: the journals before this one may say what that is (lastStationIn).
    bool takeUnmatched(uint64_t* bodyHash) noexcept {
        if (!unmatched_) return false;
        unmatched_ = false;
        if (bodyHash) *bodyHash = unmatchedHash_;
        return true;
    }
    // What the older journals said of it.
    template <class OnChange>
    void adopt(const Saved& s, OnChange&& onChange) {
        saved_ = s;
        savedChanged_ = true;
        set(s.kind, onChange);
    }

    // The next bytes of the journal, in any pieces. onChange(Kind) is called each time the type changes, in stream order.
    template <class OnChange>
    void feed(const char* p, uint32_t n, OnChange&& onChange) {
        if (!p) return;
        for (uint32_t i = 0; i < n; ++i) {
            const char c = p[i];
            if (c == '\n') {
                line(onChange);
                len_ = 0;
            } else if (len_ < kLineCap) {
                line_[len_++] = c;
            }
        }
    }

private:
    // The value of "key":"value" in this line. A key that only begins this way (BodyID, StationFaction) has no match: the token carries the closing quote and the colon.
    bool field(const char* token, const char** value, uint32_t* valueLen) const noexcept {
        const uint32_t t = static_cast<uint32_t>(std::strlen(token));
        if (len_ < t) return false;
        for (uint32_t i = 0; i + t <= len_; ++i) {
            if (line_[i] != token[0] || std::memcmp(line_ + i, token, t) != 0) continue;
            const uint32_t start = i + t;
            for (uint32_t j = start; j < len_; ++j) {
                if (line_[j] == '"') {
                    *value = line_ + start;
                    *valueLen = j - start;
                    return true;
                }
            }
            return false;
        }
        return false;
    }
    bool has(const char* token) const noexcept {
        const uint32_t t = static_cast<uint32_t>(std::strlen(token));
        for (uint32_t i = 0; i + t <= len_; ++i)
            if (std::memcmp(line_ + i, token, t) == 0) return true;
        return false;
    }

    template <class OnChange>
    void set(Kind k, OnChange&& onChange) {
        if (k == kind_) return;
        kind_ = k;
        onChange(k);
    }

    template <class OnChange>
    void line(OnChange&& onChange) {
        const char* ev = nullptr;
        uint32_t evLen = 0;
        if (!field("\"event\":\"", &ev, &evLen)) return;
        if (sameText(ev, evLen, "Undocked")) {
            set(Kind::kUnknown, onChange);
            return;
        }
        const bool location = sameText(ev, evLen, "Location");
        if (!location && !sameText(ev, evLen, "Docked") && !sameText(ev, evLen, "Embark") && !sameText(ev, evLen, "Disembark")) return;
        const char* type = nullptr;
        const char* name = nullptr;
        uint32_t typeLen = 0, nameLen = 0;
        if (field("\"StationType\":\"", &type, &typeLen) && field("\"StationName\":\"", &name, &nameLen)) {
            const Kind k = kindOf(type, typeLen);
            const uint64_t h = hashName(name, nameLen);
            if (saved_.nameHash != h || saved_.kind != k) {
                saved_.nameHash = h;
                saved_.kind = k;
                savedChanged_ = true;
            }
            set(k, onChange);
            return;
        }
        // A load on foot: the station's name is its Body, and the last one saved says what it is.
        const char* body = nullptr;
        uint32_t bodyLen = 0;
        if (location && has("\"OnFoot\":true") && has("\"BodyType\":\"Station\"") && field("\"Body\":\"", &body, &bodyLen)) {
            const uint64_t h = hashName(body, bodyLen);
            const bool match = saved_.nameHash != 0 && saved_.nameHash == h;
            set(match ? saved_.kind : Kind::kUnknown, onChange);
            if (!match) {
                unmatched_ = true;
                unmatchedHash_ = h;
            }
            return;
        }
        // At a port on a planet: see the head of this file.
        if (!location && surfaceKind(kind_) && has("\"OnPlanet\":true")) return;
        if (location && has("\"OnFoot\":true") && has("\"BodyType\":\"Planet\"")) {
            set(surfaceKind(saved_.kind) ? saved_.kind : Kind::kUnknown, onChange);
            return;
        }
        set(Kind::kUnknown, onChange);   // in space, on a planet, or an event at no station
    }

    char line_[kLineCap] = {};
    uint32_t len_ = 0;
    Kind kind_ = Kind::kUnknown;
    Saved saved_;
    bool savedChanged_ = false;
    bool unmatched_ = false;
    uint64_t unmatchedHash_ = 0;
};

// The last station a stretch of journal names, and whether it names one. A commander on foot in a station disembarked there, so the newest journal that names any station says which.
inline bool lastStationIn(const char* text, uint32_t n, Saved* out) {
    Latch latch;
    latch.feed(text, n, [](Kind) {});
    if (latch.saved().nameHash == 0) return false;
    if (out) *out = latch.saved();
    return true;
}

}  // namespace journal_station
}  // namespace edvr
