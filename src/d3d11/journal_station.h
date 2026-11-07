// The type of station the commander is at, read from the journal as its bytes stream past. Pure (no game, no Windows), so tools\optimized_station_test runs it directly;
// journal_watch.cpp feeds it every byte the tail reads, once, on the worker thread. The concourse cull's bake is chosen by it (concourse_cull_core.h, bakePlanFor).
//
// WHAT THE JOURNAL SAYS (3,097 journals, 2026-11-07). Docked, Embark, Disembark and a docked Location name the station: "StationName", "StationType", "MarketID". A Location written
// when the game loads on foot never does (580 of 580): it has only "Body":"<the station's name>" and "BodyType":"Station". So the last station named is saved, and a load on foot
// matches its Body against that. On a carrier Body is the star or planet, so a load on foot there reads unknown. Unknown is "not confirmed": the game's own station.
// A Stronghold Carrier is "StationType":"SurfaceStation", told apart only by its name. "Bernal" is the journal's other name for an Ocellus: Location has written it where Docked says
// Ocellus (INFERRED, from the two lists; no one station compared).
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
            set(saved_.nameHash != 0 && saved_.nameHash == hashName(body, bodyLen) ? saved_.kind : Kind::kUnknown, onChange);
            return;
        }
        set(Kind::kUnknown, onChange);   // in space, on a planet, or an event at no station
    }

    char line_[kLineCap] = {};
    uint32_t len_ = 0;
    Kind kind_ = Kind::kUnknown;
    Saved saved_;
    bool savedChanged_ = false;
};

}  // namespace journal_station
}  // namespace edvr
