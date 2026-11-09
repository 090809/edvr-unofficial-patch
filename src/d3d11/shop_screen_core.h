// F209 (native on-foot VR): the Pioneer Supplies buy screens, found by the game's own open flag. The pure half, header only, no Windows and no game: native_frame_test runs these
// functions, native_on_foot_hooks_test runs them against the REAL glue (native_on_foot_hooks.cpp, the five hooks below), and the Animate post-hook's F107 rule takes the signal from
// native_on_foot_hooks.h's nativeOnFootShopOpen().
//
// WHY. At the suit shop the game draws the commander in a showroom from a camera of its own, and at the weapon shop a preview in front of the camera; EDVR took each camera for the
// commander's eye (F208, F208b: the pin, the raise, the hold, the settle) unless its distance from the neck said otherwise. A reverse-engineering pass on build 334925
// (analysis\decomp\verify_20261101_shop_flag.txt) found the game's own flag: SuitVendorInteraction (vtable 0x5255D20) and WeaponsVendorInteraction (0x5255A20), the two buy screens, derive
// from the UI interaction base whose Show (0x9C3960, vtable +0x70) sets the byte at +0x79 to 1 and whose Close (0x9C22A0, +0x80) calls OnClose and clears it; 0x9C3290 (+0x78) is the game's own
// IsOpen for that byte. CONFIRMED by the code: the byte; CANDIDATE until flown: that these screens are the showrooms the pilot saw.
//
// THE HOOKS (build 334925 only; the shared PE identity and the prologue bytes below gate each one; any mismatch stands that hook down with one line and patches nothing):
//   Show     0x9C3960  shared by 40+ classes: the callback tests *(this) against the two vtables and returns on everything else; for ours it reads +0x79 before and after the original.
//   Close    0x9C22A0  likewise.
//   SetState 0xA237B0  (rcx = the state machine's handle, rdx = the new state's C string): records the new state when the handle's name is "SuitUIStateMachine".
//   Kiosk    0x1973820 / 0x1916440  ArmouryVendorActivity OnStart / OnStop (vtable 0x51E9770): the kiosk session, byte +0x1F8.
// The Show/Close pair is hooked once on the shared functions (the per-class OnOpen/OnClose would be four hooks for the same two events and 0x1F344E0's prologue differs between builds).
//
// THE SIGNAL. A buy screen is open from its Show to its Close. The hooks remember `this` for each class; once a frame, on the frame thread, the remembered object's vtable is read (guarded) and
// compared with the class's, and the byte +0x79 is read (guarded): so a missed Close, a freed object or a reused address cannot leave the signal stuck. The answer is handed to the Animate
// post-hook (where F107 decides) through an atomic.
//
// F228 (TEMPORARY A/B): the two Livery screens, the suit's and the weapon's. In VR on foot the Livery screen's commander preview is placed wrongly on one headset, and right while F107 holds the
// camera away (docs\vr-livery-model-placement-2026-11-09.md: the RE and the Pimax visit). The same reverse-engineering pass (build 334925) found that HumanoidLiveryActivity (vtable 0x5612F30) and
// HumanoidWeaponLiveryActivity (0x5614328) share the buy screens' Show, IsOpen and Close (their vtable slots +0x70, +0x78 and +0x80 hold 0x9C3960, 0x9C3290 and 0x9C22A0; checked in the exe) and the open
// byte +0x79. So the hooks above already run for them: the Show/Close filter now also knows these two vtables, and the frame thread re-reads them like the buy screens. While a Livery screen reads open
// AND experimental.native_on_foot_livery_game_view is on (the default), the shop signal is on and F107 holds the camera away, as it does at a buy screen; with the key off the screens are only watched
// (Show, Close and the byte are logged, nothing is held). The key is a switch for one flight and goes when the arc closes.
//
// F229 (read-only instrument): why the Livery screen's preview camera ends up at the commander. Each of these screens embeds a CustomisationVisual preview client (constructor 0x126B670,
// called from the screen's own constructor at the offset clientOffset() gives, read from the exe); the camera's state sits in the client: the setup mode (+0x398), the camera kind (+0x39C: 0
// node distance, 1 avatar-bounds distance, 2 none), the zoom step (+0x570), the current orbit distance (+0x580), the maximum (+0x590) and minimum (+0x5A0) distances, the retry flag (+0x5B0: 1
// means the game recomputes the distance on its next update) and the pose matrix (+0x5E0). Once a second, for each such screen that reads open, the frame thread reads them (guarded) and logs
// them; and an exact-build hook on the client's scene setup (0x12714C0: it picks the scene's attach and camera nodes from a mode int read through its third argument; its rcx is the client + 0x58)
// logs each call and which watched screen's client rcx - 0x58 is. Nothing is written and nothing the game does changes. HumanoidOutfittingInteraction (the loadout browser, whose preview camera
// is right) is watched the same way for comparison: it shares the base's Show and Close, and never joins the signal.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace shop {

// ---- build 334925's addresses and prologues ---------------------------------------------------------------------------------------------------------------------------------
constexpr uintptr_t kShowRva = 0x9C3960, kCloseRva = 0x9C22A0, kSetStateRva = 0xA237B0, kKioskStartRva = 0x1973820, kKioskStopRva = 0x1916440;
constexpr uintptr_t kSuitVtableRva = 0x5255D20, kWeaponsVtableRva = 0x5255A20, kKioskVtableRva = 0x51E9770;
constexpr uintptr_t kLiveryVtableRva = 0x5612F30, kWeaponLiveryVtableRva = 0x5614328;   // F228: HumanoidLiveryActivity and HumanoidWeaponLiveryActivity (Show, IsOpen, Close and the byte +0x79 are the buy screens')
constexpr uintptr_t kOutfittingVtableRva = 0x524D908;   // F229: HumanoidOutfittingInteraction, the loadout browser (watched only)
constexpr char kLiveryKey[] = "experimental.native_on_foot_livery_game_view";
constexpr uint32_t kOffOpen = 0x79;        // the interaction's "my screen is open" byte (the base class's Show sets it, Close clears it)
constexpr uint32_t kOffKiosk = 0x1F8;      // ArmouryVendorActivity's session byte (OnStart sets it, OnStop clears it)
constexpr uint32_t kOffHandleName = 0x08;  // the state machine handle's pointer to a refcounted Cobra string
constexpr uint32_t kOffStringChars = 0x14; // ... whose characters start here
constexpr char kStateMachineName[] = "SuitUIStateMachine";

// Each prologue ends before its first rip-relative or relative operand (the call or the displacement is not compared). CodeHook steals the whole instructions that cover five bytes: Show,
// Close and SetState `40 53 | 48 83 EC 20` (six), the kiosk's start `48 8B C4 | 55 | 53` (five) and stop `48 89 5C 24 10` (five).
constexpr size_t kShowPrologueBytes = 14;
inline constexpr uint8_t kShowPrologue[kShowPrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x70, 0xE8};
constexpr size_t kClosePrologueBytes = 15;
inline constexpr uint8_t kClosePrologue[kClosePrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x79, 0x79, 0x00, 0x48, 0x8B, 0xD9, 0x74, 0x1B};
constexpr size_t kSetStatePrologueBytes = 10;
inline constexpr uint8_t kSetStatePrologue[kSetStatePrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0xE8};
constexpr size_t kKioskStartPrologueBytes = 16;
inline constexpr uint8_t kKioskStartPrologue[kKioskStartPrologueBytes] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00};
constexpr size_t kKioskStopPrologueBytes = 16;
inline constexpr uint8_t kKioskStopPrologue[kKioskStopPrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x81, 0xC1};

// F229: the preview client's scene setup (0x12714C0, a virtual member: rcx the object it runs on, rdx a scene handle, r8 a pointer to the mode int; no stack arguments). The first 16 bytes
// `mov r11,rsp | push rbp | push r12 | push r15 | lea rbp,[r11-768h]` and the first byte of `sub rsp,850h` carry no relative operand; CodeHook steals the first six (`mov r11,rsp | push rbp | push r12`).
constexpr uintptr_t kSetupRva = 0x12714C0;
constexpr size_t kSetupPrologueBytes = 16;
inline constexpr uint8_t kSetupPrologue[kSetupPrologueBytes] = {0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54, 0x41, 0x57, 0x49, 0x8D, 0xAB, 0x98, 0xF8, 0xFF, 0xFF, 0x48};
// The preview client's camera fields, relative to the client. The setup is reached through the client's interface at +0x58 (the vtable 0x51AA4B8 slot 3, installed there by the client's constructor),
// so its rcx is the client + 0x58: kClientInterface is subtracted before the client is looked for.
constexpr uint32_t kOffMode = 0x398, kOffKind = 0x39C, kOffStep = 0x570, kOffDist = 0x580, kOffMax = 0x590, kOffMin = 0x5A0, kOffRetry = 0x5B0, kOffPose = 0x5E0, kClientInterface = 0x58;
constexpr uint32_t kSetupObjectSpan = 0x1000;   // a pointer at most this far past a watched screen's object is "in" that object

// ---- the classes -----------------------------------------------------------------------------------------------------------------------------------------------------------
enum Class : int { kSuit = 0, kWeapons = 1, kKiosk = 2, kLivery = 3, kWeaponLivery = 4, kOutfitting = 5, kClassCount = 6, kNotOurs = -1 };   // F229: kOutfitting is watched only
inline const char* className(int c) noexcept {
    return c == kSuit ? "SuitVendorInteraction" : c == kWeapons ? "WeaponsVendorInteraction" : c == kKiosk ? "ArmouryVendorActivity" : c == kLivery ? "HumanoidLiveryActivity"
         : c == kWeaponLivery ? "HumanoidWeaponLiveryActivity" : c == kOutfitting ? "HumanoidOutfittingInteraction" : "another class";
}
inline uintptr_t vtableRva(int c) noexcept {
    return c == kSuit ? kSuitVtableRva : c == kWeapons ? kWeaponsVtableRva : c == kKiosk ? kKioskVtableRva : c == kLivery ? kLiveryVtableRva : c == kWeaponLivery ? kWeaponLiveryVtableRva
         : c == kOutfitting ? kOutfittingVtableRva : 0;
}
inline bool isLivery(int c) noexcept { return c == kLivery || c == kWeaponLivery; }
// F229: where the CustomisationVisual preview client lies in each class's object, from the constructors' `lea rcx,[object+X]` before the call to 0x126B670 (read from build 334925's exe):
// the two buy screens 0x448, the suit Livery 0x680, the weapon Livery 0x630, the loadout browser 0xC0. 0 for a class with no client (the kiosk).
inline uint32_t clientOffset(int c) noexcept {
    return c == kSuit || c == kWeapons ? 0x448u : c == kLivery ? 0x680u : c == kWeaponLivery ? 0x630u : c == kOutfitting ? 0xC0u : 0u;
}
// Which of the two buy screens a vtable pointer belongs to (the Show/Close hook's whole filter); kNotOurs for any other class of the 40 that share the base.
inline int buyScreenOfVtable(uint64_t vtable, uintptr_t base) noexcept {
    return vtable == base + kSuitVtableRva ? kSuit : vtable == base + kWeaponsVtableRva ? kWeapons : kNotOurs;
}
// F228: which of the four screens that share the base's Show and Close (the two buy screens and the two Livery screens) a vtable pointer belongs to: the hook's whole filter.
inline int screenOfVtable(uint64_t vtable, uintptr_t base) noexcept {
    const int buy = buyScreenOfVtable(vtable, base);
    return buy != kNotOurs ? buy : vtable == base + kLiveryVtableRva ? kLivery : vtable == base + kWeaponLiveryVtableRva ? kWeaponLivery : vtable == base + kOutfittingVtableRva ? kOutfitting : kNotOurs;
}

// ---- the once-a-frame re-check ---------------------------------------------------------------------------------------------------------------------------------------------
// What one remembered object reads as, from guarded reads the caller made: whether there is an object, whether its vtable and byte could be read, the vtable, the byte.
enum class Verdict : int { kNone = 0, kOpen = 1, kClosed = 2, kStaleVtable = 3, kUnreadable = 4 };
inline Verdict judgeObject(bool haveObject, bool vtableRead, uint64_t vtable, uint64_t expectedVtable, bool byteRead, uint8_t byte) noexcept {
    if (!haveObject) return Verdict::kNone;
    if (!vtableRead) return Verdict::kUnreadable;
    if (vtable != expectedVtable) return Verdict::kStaleVtable;   // freed and reused (or a different class at the address): the object is forgotten
    if (!byteRead) return Verdict::kUnreadable;
    return byte != 0 ? Verdict::kOpen : Verdict::kClosed;        // the game's own tests are `!= 0`
}
// A verdict that makes the frame thread forget the object (so it can never be read again until a Show remembers one).
inline bool forgets(Verdict v) noexcept { return v == Verdict::kStaleVtable || v == Verdict::kUnreadable; }
inline const char* verdictText(Verdict v) noexcept {
    switch (v) {
        case Verdict::kNone: return "none remembered";
        case Verdict::kOpen: return "open";
        case Verdict::kClosed: return "closed";
        case Verdict::kStaleVtable: return "forgotten: its vtable is no longer the class's";
        default: return "forgotten: its memory could not be read";
    }
}

// The signal the rest of EDVR reads: a buy screen (either) is open.
struct Signal {
    bool open[2] = {false, false};   // the suit and the weapon buy screen
    bool kiosk = false;              // ArmouryVendorActivity's session (not a buy screen: the home menu; it does not hold F107 away)
    bool livery[2] = {false, false}; // F228: the suit and the weapon Livery screen (index 0 is kLivery, 1 is kWeaponLivery)
    bool buyScreenOpen() const noexcept { return open[0] || open[1]; }
    bool liveryOpen() const noexcept { return livery[0] || livery[1]; }
    // The signal F107 reads: a buy screen, or (F228) a Livery screen while the key says the game's own view holds there.
    bool holds(bool liveryGameView) const noexcept { return buyScreenOpen() || (liveryGameView && liveryOpen()); }
};
inline Signal signalOf(Verdict suit, Verdict weapons, Verdict kiosk, Verdict livery = Verdict::kNone, Verdict weaponLivery = Verdict::kNone) noexcept {
    Signal s;
    s.open[kSuit] = suit == Verdict::kOpen;
    s.open[kWeapons] = weapons == Verdict::kOpen;
    s.kiosk = kiosk == Verdict::kOpen;
    s.livery[0] = livery == Verdict::kOpen;
    s.livery[1] = weaponLivery == Verdict::kOpen;
    return s;
}

// ---- the state machine ------------------------------------------------------------------------------------------------------------------------------------------------------
// Is this (bounded) copy of the handle's name string the one machine we record? The hook reads at most kNameCopy bytes and always terminates them.
constexpr size_t kNameCopy = 24, kStateCopy = 40;
inline bool isSuitUiStateMachine(const char* name) noexcept { return name && std::strncmp(name, kStateMachineName, sizeof(kStateMachineName)) == 0; }
// A vendor state ("Armoury Vendor", "Suit Vendor", "Weapons Vendor", "Suit Upgrade Vendor", "Weapons Upgrade Vendor"): the once-a-second line runs while the machine is in one, so a machine left in some
// other state the build does not name (a ship, a menu) never keeps the log writing a line a second for the rest of the session.
inline bool stateIsVendor(const char* state) noexcept { return state && std::strstr(state, "Vendor") != nullptr; }
// Is anything the once-a-second line reports set: a buy screen open, the kiosk session running, the machine in a vendor state, or (F228) a Livery screen open.
inline bool anySet(bool buyScreenOpen, bool kiosk, const char* state, bool liveryOpen = false) noexcept { return buyScreenOpen || kiosk || stateIsVendor(state) || liveryOpen; }
// At most one line a second.
struct Pacer {
    bool have = false;
    uint64_t lastMs = 0;
    bool due(uint64_t nowMs) noexcept {
        if (have && nowMs - lastMs < 1000) return false;
        have = true;
        lastMs = nowMs;
        return true;
    }
};

// ---- the lines ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Every line starts with this, so a flight log is read with one grep (and --health finds the lines by it).
inline const char* tag() noexcept { return "shop screens (F209):"; }
enum Kind : uint8_t { kShow = 1, kClose = 2, kState = 3, kKioskStart = 4, kKioskStop = 5 };
struct Event {
    uint8_t kind = 0;
    int8_t cls = kNotOurs;
    int8_t before = -1, after = -1;     // +0x79 (or +0x1F8) before and after the original; -1 unreadable
    uint32_t tid = 0;
    uint32_t thisLow = 0;
    char from[kStateCopy] = {};
    char to[kStateCopy] = {};
};
inline int formatEvent(char* buf, size_t cap, const Event& e) noexcept {
    switch (e.kind) {
        case kShow:
        case kClose:
            return std::snprintf(buf, cap, "%s %s %s: +0x79 %d -> %d (this 0x%08X, thread %u)", tag(), className(e.cls), e.kind == kShow ? "Show" : "Close", e.before, e.after, e.thisLow, e.tid);
        case kState:
            return std::snprintf(buf, cap, "%s SuitUIStateMachine: \"%s\" -> \"%s\" (thread %u)", tag(), e.from, e.to, e.tid);
        case kKioskStart:
        case kKioskStop:
            return std::snprintf(buf, cap, "%s %s %s: +0x1F8 %d -> %d (this 0x%08X, thread %u)", tag(), className(kKiosk), e.kind == kKioskStart ? "started" : "stopped", e.before, e.after, e.thisLow, e.tid);
        default:
            return std::snprintf(buf, cap, "%s an event of kind %u", tag(), static_cast<unsigned>(e.kind));
    }
}
inline const char* liveryText(const Signal& s) noexcept { return s.livery[0] && s.livery[1] ? "suit and weapon open" : s.livery[0] ? "suit open" : s.livery[1] ? "weapon open" : "closed"; }
inline int formatSignal(char* buf, size_t cap, const Signal& s, bool on, uint32_t seq) noexcept {
    return std::snprintf(buf, cap, "%s the shop signal is %s (suit %s, weapons %s, kiosk %s, livery %s; change %u)", tag(), on ? "ON: F107 holds the camera away" : "OFF", s.open[kSuit] ? "open" : "closed",
                         s.open[kWeapons] ? "open" : "closed", s.kiosk ? "running" : "not running", liveryText(s), seq);
}
// F228: said once when a Livery screen opens: whether the key makes it hold the camera away or it is only watched.
inline int formatLiveryOpened(char* buf, size_t cap, int cls, bool holds) noexcept {
    return std::snprintf(buf, cap, "%s %s opened: %s (%s is %s)", tag(), className(cls), holds ? "F107 holds the camera away, the view is the game's own" : "only watched, the camera is not held away",
                         kLiveryKey, holds ? "on" : "off");
}
inline int formatForgotten(char* buf, size_t cap, int cls, Verdict v, uint64_t vtable, uint64_t expected, uint32_t thisLow) noexcept {
    return std::snprintf(buf, cap, "%s the remembered %s (this 0x%08X) is %s (vtable 0x%llX, the class's 0x%llX): the signal does not hold on it", tag(), className(cls), thisLow, verdictText(v),
                         static_cast<unsigned long long>(vtable), static_cast<unsigned long long>(expected));
}
// The once-a-second line while anything is set: the remembered objects' bytes ("none" when none is remembered), the state, the kiosk byte, the signal, and F107's state (text from its owner).
inline const char* byteText(int b) noexcept { return b < 0 ? "none" : b == 0 ? "0" : "1"; }
inline int formatSecond(char* buf, size_t cap, int suitByte, int weaponsByte, int kioskByte, int liveryByte, int weaponLiveryByte, const char* state, bool signal, const char* f107) noexcept {
    return std::snprintf(buf, cap, "%s 1 s: suit +0x79 %s, weapons +0x79 %s, kiosk +0x1F8 %s, livery +0x79 %s, weapon livery +0x79 %s, SuitUIStateMachine \"%s\", signal %s; F107: %s", tag(),
                         byteText(suitByte), byteText(weaponsByte), byteText(kioskByte), byteText(liveryByte), byteText(weaponLiveryByte), state && state[0] ? state : "unknown", signal ? "on" : "off", f107);
}

// ---- the log's cap ----------------------------------------------------------------------------------------------------------------------------------------------------------
// Events are edges, but a flapping screen must not fill the log: at most kLinesPer10s event lines in any ten seconds; the rest are counted and said once.
constexpr uint32_t kLinesPer10s = 40;
struct LineCap {
    uint64_t windowMs = 0;
    uint32_t written = 0, dropped = 0;
    // True when the line may be written now. A window that ended with drops says so in `*summary` (the number dropped), once.
    bool allow(uint64_t nowMs, uint32_t* summary) noexcept {
        *summary = 0;
        if (nowMs - windowMs >= 10000 || windowMs == 0) {
            *summary = dropped;
            windowMs = nowMs;
            written = 0;
            dropped = 0;
        }
        if (written >= kLinesPer10s) {
            ++dropped;
            return false;
        }
        ++written;
        return true;
    }
};

// ---- F229: the preview client's camera fields and its setup calls ---------------------------------------------------------------------------------------------------------------
// Every line starts with this (--health finds them by it); the class name follows.
inline const char* camTag() noexcept { return "livery camera (F229):"; }
// The fields as the frame thread read them: the setup mode, the camera kind, the zoom step, the current, maximum and minimum orbit distances, the retry flag and the 16 floats of the pose matrix.
struct CamFields {
    int32_t mode = 0, kind = 0;
    float step = 0.0f, dist = 0.0f, maxDist = 0.0f, minDist = 0.0f;
    uint8_t retry = 0;
    float pose[16] = {};
};
inline CamFields camFromBytes(const uint8_t modeKind[8], const uint8_t step[4], const uint8_t dist[4], const uint8_t maxDist[4], const uint8_t minDist[4], uint8_t retry, const uint8_t pose[64]) noexcept {
    CamFields f;
    std::memcpy(&f.mode, modeKind, 4);
    std::memcpy(&f.kind, modeKind + 4, 4);
    std::memcpy(&f.step, step, 4);
    std::memcpy(&f.dist, dist, 4);
    std::memcpy(&f.maxDist, maxDist, 4);
    std::memcpy(&f.minDist, minDist, 4);
    f.retry = retry;
    std::memcpy(f.pose, pose, 64);
    return f;
}
// The four floats and the pose are floats the game keeps; a value that is not a number says the offsets are not what the RE took them for.
inline bool camFinite(const CamFields& f) noexcept {
    if (!std::isfinite(f.step) || !std::isfinite(f.dist) || !std::isfinite(f.maxDist) || !std::isfinite(f.minDist)) return false;
    for (float v : f.pose)
        if (!std::isfinite(v)) return false;
    return true;
}
// A float as text: four decimals, "nan" or "inf" for what is not a number, an exponent past a billion (so one wild value cannot swell the line).
inline void floatText(char* out, size_t cap, float v) noexcept {
    if (std::isnan(v)) std::snprintf(out, cap, "%s", "nan");
    else if (std::isinf(v)) std::snprintf(out, cap, "%s", v < 0 ? "-inf" : "inf");
    else if (std::fabs(v) >= 1.0e9f) std::snprintf(out, cap, "%.3e", static_cast<double>(v));
    else std::snprintf(out, cap, "%.4f", static_cast<double>(v));
}
inline float vectorLength(float x, float y, float z) noexcept { return std::sqrt(x * x + y * y + z * z); }
// The once-a-second line for one open screen: its object and client (low 32 bits), the named fields, the pose as four rows, and the length of the matrix's translation read both ways (the last
// row, the last column) since the layout is not known.
inline int formatCamera(char* buf, size_t cap, int cls, uint32_t thisLow, uint32_t clientLow, const CamFields& f) noexcept {
    char st[24], di[24], mx[24], mn[24], m[16][24], rowLen[24], colLen[24];
    floatText(st, sizeof(st), f.step);
    floatText(di, sizeof(di), f.dist);
    floatText(mx, sizeof(mx), f.maxDist);
    floatText(mn, sizeof(mn), f.minDist);
    for (int i = 0; i < 16; ++i) floatText(m[i], sizeof(m[i]), f.pose[i]);
    floatText(rowLen, sizeof(rowLen), vectorLength(f.pose[12], f.pose[13], f.pose[14]));
    floatText(colLen, sizeof(colLen), vectorLength(f.pose[3], f.pose[7], f.pose[11]));
    return std::snprintf(buf, cap,
                         "%s %s 1 s: this 0x%08X, client 0x%08X: mode %d, kind %d, step %s, dist %s, max %s, min %s, retry %u, pose rows [%s %s %s %s] [%s %s %s %s] [%s %s %s %s] [%s %s %s %s], "
                         "translation length last row %s, last column %s",
                         camTag(), className(cls), thisLow, clientLow, f.mode, f.kind, st, di, mx, mn, static_cast<unsigned>(f.retry), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
                         m[10], m[11], m[12], m[13], m[14], m[15], rowLen, colLen);
}
// Said once per class: the client's memory could not be read, or holds a value that is not a number.
inline int formatCameraUnreadable(char* buf, size_t cap, int cls, uint32_t thisLow, uint32_t clientLow, bool unreadable) noexcept {
    return std::snprintf(buf, cap, "%s %s (this 0x%08X) client 0x%08X: the camera fields %s: the offsets +0x398 +0x39C +0x570 +0x580 +0x590 +0x5A0 +0x5B0 +0x5E0 are not this object's camera", camTag(),
                         className(cls), thisLow, clientLow, unreadable ? "could not be read" : "hold a value that is not a number");
}
// A pointer's distance past an object's start, when it lies within kSetupObjectSpan of it.
inline bool locateInObject(uintptr_t p, uintptr_t obj, int32_t* delta) noexcept {
    if (obj == 0 || p < obj || p - obj >= kSetupObjectSpan) return false;
    *delta = static_cast<int32_t>(p - obj);
    return true;
}
// One call of the setup, as the hook saw it. rcx on entry is the client + 0x58: objLow is rcx - 0x58, the CustomisationVisual object.
struct SetupCall {
    uint32_t tid = 0;
    uint32_t rcxLow = 0, objLow = 0, r8Low = 0;
    bool modeRead = false;      // *(int*)r8 could be read
    int32_t mode = 0;
    int8_t cls = kNotOurs;      // the watched screen whose object rcx - 0x58 lies in (kNotOurs: none)
    int32_t delta = 0;          // rcx - 0x58 minus that screen's object
};
inline int formatSetup(char* buf, size_t cap, const SetupCall& c) noexcept {
    char where[120];
    if (c.cls == kNotOurs) {
        std::snprintf(where, sizeof(where), "in no watched screen's object");
    } else {
        const int32_t rel = c.delta - static_cast<int32_t>(clientOffset(c.cls));
        std::snprintf(where, sizeof(where), "%s object +0x%X, which is client %c0x%X", className(c.cls), static_cast<unsigned>(c.delta), rel < 0 ? '-' : '+', static_cast<unsigned>(rel < 0 ? -rel : rel));
    }
    char mode[24];
    if (c.modeRead) std::snprintf(mode, sizeof(mode), "%d", c.mode);
    else std::snprintf(mode, sizeof(mode), "unreadable");
    return std::snprintf(buf, cap, "%s setup call 0x12714C0: rcx 0x%08X, object (rcx - 0x58) 0x%08X, r8 0x%08X, mode %s (thread %u), the object is %s", camTag(), c.rcxLow, c.objLow, c.r8Low, mode, c.tid,
                         where);
}
// A window that dropped lines says so once; `what` is "camera" or "setup".
inline int formatCamCapped(char* buf, size_t cap, const char* what, uint32_t dropped) noexcept {
    return std::snprintf(buf, cap, "%s %u %s lines in the last 10 s were not written (at most %u in any ten seconds)", camTag(), dropped, what, kLinesPer10s);
}
inline int formatSetupLost(char* buf, size_t cap, uint64_t lost) noexcept {
    return std::snprintf(buf, cap, "%s %llu setup call(s) were not recorded (the ring was full or a second hook thread was in it)", camTag(), static_cast<unsigned long long>(lost));
}

}  // namespace shop
}  // namespace edvr
