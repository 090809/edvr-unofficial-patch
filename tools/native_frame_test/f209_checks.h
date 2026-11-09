// F209 checks (native on-foot VR): the Pioneer Supplies buy screens found by the game's own open flag, and F107's rule that a buy screen open holds the camera away at any distance.
// Header only, run by native_frame_test (run209). The pure half only: the constants, the once-a-frame re-check's verdicts, the signal, F107's rule (native_on_foot_f107.h Away::step with
// the shop input), the lines and their cap, and the wiring read as text. The real glue (the five hooks over synthetic functions with the game's prologues, the callbacks' reads, the ring, the
// frame thread's tick and its lines) is run by native_on_foot_hooks_test.
//
// The figures are the flights' (edvr_gfx_20261101_114724 and _141044, v0.22.0-rc.5): the suit shop's showroom camera 3.6 to 3.7 m from the neck with a second camera 46 cm over it taking turns at
// 45 calls a second; the weapon showroom's camera the commander's own, 0.9 cm under the neck (no distance test could see it).
#pragma once

#include "../../src/d3d11/native_on_foot_f107.h"
#include "../../src/d3d11/preview_camera_core.h"
#include "../../src/d3d11/shop_screen_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f209test {

namespace shop = edvr::shop;
namespace f107 = edvr::nofoot::f107;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());   // a checkout with CRLF reads as LF
    return s;
}
inline bool has_(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

// What a remembered object looks like to the frame thread's guarded reads: whether its vtable and its byte could be read, and their values.
struct FakeObject {
    bool vtableReadable = true, byteReadable = true;
    uint64_t vtable = 0;
    uint8_t byte = 0;
};
inline shop::Verdict recheck(const FakeObject* o, int cls, uintptr_t base) {
    if (!o) return shop::judgeObject(false, false, 0, 0, false, 0);
    return shop::judgeObject(true, o->vtableReadable, o->vtable, base + shop::vtableRva(cls), o->byteReadable, o->byte);
}

template <class Check>
inline void run209(Check&& check) {
    const uintptr_t base = 0x7FF600000000ull;
    // ---- 1. the constants: build 334925's addresses, offsets and prologues (the RE note's, read from the exe) ----------------------------------------------------------------------------
    {
        check(shop::kShowRva == 0x9C3960 && shop::kCloseRva == 0x9C22A0 && shop::kSetStateRva == 0xA237B0 && shop::kKioskStartRva == 0x1973820 && shop::kKioskStopRva == 0x1916440 &&
                  shop::kSuitVtableRva == 0x5255D20 && shop::kWeaponsVtableRva == 0x5255A20 && shop::kKioskVtableRva == 0x51E9770 && shop::kOffOpen == 0x79 && shop::kOffKiosk == 0x1F8,
              "F209: the addresses are build 334925's (Show 0x9C3960, Close 0x9C22A0, SetState 0xA237B0, kiosk start 0x1973820 and stop 0x1916440; vtables suit 0x5255D20, weapons 0x5255A20, kiosk 0x51E9770; bytes +0x79 and +0x1F8)");
        check(shop::kLiveryVtableRva == 0x5612F30 && shop::kWeaponLiveryVtableRva == 0x5614328 && shop::kClassCount == 6 && shop::kLivery == 3 && shop::kWeaponLivery == 4 && shop::kOutfitting == 5 &&
                  std::strcmp(shop::kLiveryKey, "experimental.native_on_foot_livery_game_view") == 0,
              "F228: the Livery screens' vtables are build 334925's (HumanoidLiveryActivity 0x5612F30, HumanoidWeaponLiveryActivity 0x5614328; their Show, IsOpen and Close slots hold the buy screens' functions), six classes (F229 adds the loadout browser), the key's name");
        check(shop::kShowPrologueBytes == 14 && shop::kShowPrologue[0] == 0x40 && shop::kShowPrologue[1] == 0x53 && shop::kShowPrologue[13] == 0xE8 && shop::kClosePrologueBytes == 15 &&
                  shop::kClosePrologue[6] == 0x80 && shop::kClosePrologue[7] == 0x79 && shop::kClosePrologue[8] == 0x79 && shop::kSetStatePrologueBytes == 10 && shop::kSetStatePrologue[9] == 0xE8 &&
                  shop::kKioskStartPrologueBytes == 16 && shop::kKioskStartPrologue[0] == 0x48 && shop::kKioskStartPrologue[2] == 0xC4 && shop::kKioskStopPrologueBytes == 16 &&
                  shop::kKioskStopPrologue[0] == 0x48 && shop::kKioskStopPrologue[4] == 0x10,
              "F209: the prologues are the exe's (Show 14 bytes ending before its call, Close 15 with `cmp byte [rcx+79h],0`, SetState 10, the kiosk's 16 and 16)");
        check(std::strcmp(shop::kStateMachineName, "SuitUIStateMachine") == 0 && shop::kOffHandleName == 0x08 && shop::kOffStringChars == 0x14, "F209: the state machine handle's name is read at [handle+8]+0x14 and is SuitUIStateMachine");
    }
    // ---- 2. the Show/Close filter: the two buy screens by their vtables, every other class of the 40 that share the base is returned at once -----------------------------------------------------------
    {
        const uint64_t suit = base + shop::kSuitVtableRva, weapons = base + shop::kWeaponsVtableRva, kiosk = base + shop::kKioskVtableRva, other = base + 0x5255000;
        check(shop::buyScreenOfVtable(suit, base) == shop::kSuit && shop::buyScreenOfVtable(weapons, base) == shop::kWeapons && shop::buyScreenOfVtable(kiosk, base) == shop::kNotOurs &&
                  shop::buyScreenOfVtable(other, base) == shop::kNotOurs && shop::buyScreenOfVtable(0, base) == shop::kNotOurs && shop::buyScreenOfVtable(suit, base + 0x1000) == shop::kNotOurs,
              "F209: Show and Close are filtered on *(this): the suit and the weapon buy screens' vtables are ours; the kiosk, any other class, a null vtable and another image base are not");
        check(shop::vtableRva(shop::kSuit) == shop::kSuitVtableRva && shop::vtableRva(shop::kWeapons) == shop::kWeaponsVtableRva && shop::vtableRva(shop::kKiosk) == shop::kKioskVtableRva &&
                  std::strcmp(shop::className(shop::kSuit), "SuitVendorInteraction") == 0 && std::strcmp(shop::className(shop::kWeapons), "WeaponsVendorInteraction") == 0 &&
                  std::strcmp(shop::className(shop::kKiosk), "ArmouryVendorActivity") == 0,
              "F209: each class names its vtable and its own name");
    }
    {   // F228: the Livery screens join the filter; the buy screens' filter is unchanged
        const uint64_t liv = base + shop::kLiveryVtableRva, wliv = base + shop::kWeaponLiveryVtableRva, suit = base + shop::kSuitVtableRva, weapons = base + shop::kWeaponsVtableRva,
                       kiosk = base + shop::kKioskVtableRva, other = base + 0x5255000;
        check(shop::screenOfVtable(liv, base) == shop::kLivery && shop::screenOfVtable(wliv, base) == shop::kWeaponLivery && shop::screenOfVtable(suit, base) == shop::kSuit &&
                  shop::screenOfVtable(weapons, base) == shop::kWeapons && shop::screenOfVtable(kiosk, base) == shop::kNotOurs && shop::screenOfVtable(other, base) == shop::kNotOurs &&
                  shop::screenOfVtable(0, base) == shop::kNotOurs && shop::screenOfVtable(liv, base + 0x1000) == shop::kNotOurs &&
                  shop::buyScreenOfVtable(liv, base) == shop::kNotOurs && shop::buyScreenOfVtable(wliv, base) == shop::kNotOurs,
              "F228: Show and Close are filtered on *(this): the two Livery vtables are ours too (and not buy screens); the kiosk, any other class, null and another base are not");
        check(shop::vtableRva(shop::kLivery) == shop::kLiveryVtableRva && shop::vtableRva(shop::kWeaponLivery) == shop::kWeaponLiveryVtableRva &&
                  std::strcmp(shop::className(shop::kLivery), "HumanoidLiveryActivity") == 0 && std::strcmp(shop::className(shop::kWeaponLivery), "HumanoidWeaponLiveryActivity") == 0 &&
                  shop::isLivery(shop::kLivery) && shop::isLivery(shop::kWeaponLivery) && !shop::isLivery(shop::kSuit) && !shop::isLivery(shop::kWeapons) && !shop::isLivery(shop::kKiosk) && !shop::isLivery(shop::kNotOurs),
              "F228: each Livery class names its vtable and its own name");
    }
    // ---- 3. the signal from Show and Close, and the once-a-frame re-check ----------------------------------------------------------------------------------------------------------------------
    {
        FakeObject suit, weapons, kiosk;
        suit.vtable = base + shop::kSuitVtableRva;
        weapons.vtable = base + shop::kWeaponsVtableRva;
        kiosk.vtable = base + shop::kKioskVtableRva;
        auto sig = [&](const FakeObject* s, const FakeObject* w, const FakeObject* k) {
            return shop::signalOf(recheck(s, shop::kSuit, base), recheck(w, shop::kWeapons, base), recheck(k, shop::kKiosk, base));
        };
        check(!sig(nullptr, nullptr, nullptr).buyScreenOpen(), "F209: nothing remembered: the signal is off");
        suit.byte = 1;   // Show sets +0x79
        check(sig(&suit, nullptr, nullptr).buyScreenOpen() && sig(&suit, nullptr, nullptr).open[shop::kSuit] && !sig(&suit, nullptr, nullptr).open[shop::kWeapons], "F209: the suit buy screen's Show: the signal is on, for the suit");
        weapons.byte = 1;
        check(sig(&suit, &weapons, nullptr).open[shop::kSuit] && sig(&suit, &weapons, nullptr).open[shop::kWeapons], "F209: both buy screens open: both are in the signal");
        suit.byte = 0;   // Close clears it
        check(sig(&suit, &weapons, nullptr).buyScreenOpen() && !sig(&suit, &weapons, nullptr).open[shop::kSuit], "F209: the suit screen's Close: the weapon screen still holds the signal");
        weapons.byte = 0;
        check(!sig(&suit, &weapons, nullptr).buyScreenOpen(), "F209: both Closed: the signal is off");
        kiosk.byte = 1;
        const shop::Signal onlyKiosk = sig(&suit, &weapons, &kiosk);
        check(onlyKiosk.kiosk && !onlyKiosk.buyScreenOpen(), "F209: the kiosk session alone is not a buy screen: it is reported, and it does not hold F107 away");
        // a missed Close: the event never came, the game cleared the byte: the frame thread's read says closed
        suit.byte = 1;
        check(sig(&suit, nullptr, nullptr).buyScreenOpen(), "F209 (control): the byte reads open");
        suit.byte = 0;
        check(!sig(&suit, nullptr, nullptr).buyScreenOpen() && recheck(&suit, shop::kSuit, base) == shop::Verdict::kClosed && !shop::forgets(shop::Verdict::kClosed),
              "F209: a Close the hook missed cannot stick the signal: the byte read each frame says closed, and the object is kept (a screen opens again at the same address)");
        // a freed object whose address was reused: the vtable is no longer the class's
        suit.byte = 1;
        suit.vtable = base + 0x5255000;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kStaleVtable && shop::forgets(recheck(&suit, shop::kSuit, base)) && !sig(&suit, nullptr, nullptr).buyScreenOpen(),
              "F209: a stale `this` whose vtable changed (freed and reused) is forgotten and the signal goes off though its byte reads 1");
        // a vtable of the other buy screen at the same address is just as stale
        suit.vtable = base + shop::kWeaponsVtableRva;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kStaleVtable, "F209: the weapon screen's vtable at the suit's remembered address is stale for the suit");
        // memory that cannot be read
        FakeObject gone;
        gone.vtableReadable = false;
        FakeObject noByte;
        noByte.vtable = base + shop::kSuitVtableRva;
        noByte.byteReadable = false;
        check(recheck(&gone, shop::kSuit, base) == shop::Verdict::kUnreadable && recheck(&noByte, shop::kSuit, base) == shop::Verdict::kUnreadable && shop::forgets(shop::Verdict::kUnreadable) &&
                  !sig(&gone, nullptr, nullptr).buyScreenOpen() && !sig(&noByte, nullptr, nullptr).buyScreenOpen(),
              "F209: an object whose vtable or byte cannot be read is forgotten and never holds the signal");
        check(!shop::forgets(shop::Verdict::kNone) && !shop::forgets(shop::Verdict::kOpen) && !shop::forgets(shop::Verdict::kClosed), "F209: only a stale or unreadable object is forgotten");
        // the game's own tests are `!= 0`
        suit.vtable = base + shop::kSuitVtableRva;
        suit.byte = 2;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kOpen, "F209: the byte is read as the game reads it: any non-zero value is open");
    }
    {   // F228: a Livery screen open holds the signal only while the key is on; the buy screens' signal is as before either way
        FakeObject liv, wliv;
        liv.vtable = base + shop::kLiveryVtableRva;
        wliv.vtable = base + shop::kWeaponLiveryVtableRva;
        auto sig = [&](const FakeObject* l, const FakeObject* w) {
            return shop::signalOf(shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone, recheck(l, shop::kLivery, base), recheck(w, shop::kWeaponLivery, base));
        };
        check(!sig(nullptr, nullptr).holds(true) && !sig(nullptr, nullptr).holds(false) && !sig(nullptr, nullptr).liveryOpen(), "F228: no Livery screen remembered: nothing holds, key on or off");
        liv.byte = 1;   // Show sets +0x79
        const shop::Signal one = sig(&liv, nullptr);
        check(one.liveryOpen() && one.livery[0] && !one.livery[1] && !one.buyScreenOpen() && one.holds(true) && !one.holds(false),
              "F228: the suit Livery's Show: it holds the signal with the key on, and is only watched (liveryOpen, no hold) with it off; it is not a buy screen");
        wliv.byte = 1;
        const shop::Signal both = sig(&liv, &wliv);
        check(both.livery[0] && both.livery[1] && both.holds(true) && !both.holds(false) && std::strcmp(shop::liveryText(both), "suit and weapon open") == 0 && std::strcmp(shop::liveryText(one), "suit open") == 0,
              "F228: both Livery screens open are both in the signal");
        liv.byte = 0;   // Close clears it
        check(sig(&liv, &wliv).holds(true) && std::strcmp(shop::liveryText(sig(&liv, &wliv)), "weapon open") == 0, "F228: the suit Livery's Close: the weapon Livery still holds");
        wliv.byte = 0;
        check(!sig(&liv, &wliv).holds(true) && std::strcmp(shop::liveryText(sig(&liv, &wliv)), "closed") == 0, "F228: both Closed: nothing holds");
        liv.byte = 1;
        liv.vtable = base + shop::kLiveryVtableRva + 0x100;
        check(recheck(&liv, shop::kLivery, base) == shop::Verdict::kStaleVtable && !sig(&liv, nullptr).holds(true), "F228: a Livery object whose vtable changed is forgotten and holds nothing");
        liv.vtable = base + shop::kWeaponLiveryVtableRva;
        check(recheck(&liv, shop::kLivery, base) == shop::Verdict::kStaleVtable, "F228: the weapon Livery's vtable at the suit Livery's address is stale");
        FakeObject suit;
        suit.vtable = base + shop::kSuitVtableRva;
        suit.byte = 1;
        const shop::Signal buy = shop::signalOf(recheck(&suit, shop::kSuit, base), shop::Verdict::kNone, shop::Verdict::kNone);
        check(buy.holds(true) && buy.holds(false) && !buy.liveryOpen(), "F228: a buy screen holds with the key on or off: its behaviour is as in F209");
    }
    // ---- 4. F107: a buy screen open holds the camera away at any distance --------------------------------------------------------------------------------------------------------------------
    {   // the weapon showroom: the commander's own camera, 0.9 cm under the neck, 90 calls (the flight's) while the screen is open; the screen closes, the camera stays near
        f107::Away a;
        int left = 0, back = 0;
        double t = 0.0, backAt = -1.0;
        for (int i = 0; i < 20; ++i, t += 0.0222) a.step(0.2, t, false);   // walking up to the shop's counter
        const double openAt = t;
        for (int i = 0; i < 90; ++i, t += 0.0222) {
            const f107::Away::Edge e = a.step(0.009, t, true);
            left += e == f107::Away::kLeft;
            back += e == f107::Away::kBack;
        }
        const bool awayThroughout = a.away && a.shopCalls == 90 && a.distanceCalls == 0 && a.beganByShop && !a.beganByDistance && a.calls == 90 && a.farCalls == 90;
        const double closeAt = t;
        for (int i = 0; i < 200; ++i, t += 0.0222) {   // the screen closed: the camera stays near for 4.4 s
            const f107::Away::Edge e = a.step(0.009, t, false);
            left += e == f107::Away::kLeft;
            if (e == f107::Away::kBack) {
                ++back;
                backAt = t;
            }
        }
        check(left == 1 && awayThroughout && back == 1 && a.episodes == 1 && !a.away && openAt < closeAt && backAt >= closeAt + f107::kReturnHoldS - 1e-9 && backAt < closeAt + f107::kReturnHoldS + 0.03,
              "F209: the weapon showroom (the commander's own camera, 0.9 cm under the neck): the buy screen open is one episode from the first call, every call refused, and it ends through F208's hold (2.0 s) after the screen closes");
    }
    {   // the suit shop: the showroom camera 3.7 m out and the near camera 46 cm over the neck alternating for 18 s with the screen open: one episode, no flap counted as a rejoin, the hold unchanged
        f107::Away a;
        int left = 0, back = 0;
        double t = 0.0;
        for (int i = 0; i < 800; ++i, t += 1.0 / 45.0) {
            const double d = (i / 3) % 2 == 0 ? 3.7 : 0.46;
            const f107::Away::Edge e = a.step(d, t, true);
            left += e == f107::Away::kLeft;
            back += e == f107::Away::kBack;
        }
        check(left == 1 && back == 0 && a.away && a.episodes == 1 && a.rejoins == 0 && std::fabs(a.holdS - f107::kReturnHoldS) < 1e-12 && a.shopCalls == 800 && a.distanceCalls > 0 && a.distanceCalls < 800 &&
                  a.beganByShop && a.beganByDistance,
              "F209: the suit shop's alternating cameras with the screen open are one episode by both reasons, with no rejoin and the base hold (the flap that doubled the hold in F208b needs no hold here)");
        const double closeAt = t;
        double backAt = -1.0;
        for (int i = 0; i < 200; ++i, t += 1.0 / 45.0) {
            const f107::Away::Edge e = a.step(0.46, t, false);
            if (e == f107::Away::kBack) backAt = t;
        }
        check(backAt >= closeAt + f107::kReturnHoldS - 1e-9 && backAt < closeAt + f107::kReturnHoldS + 0.03 && !a.away, "F209: closing the suit screen with the camera on the commander ends the episode after the 2.0 s hold");
    }
    {   // the screen closes but the camera stays at the showroom (3.7 m): the distance test keeps the episode, as before
        f107::Away a;
        double t = 0.0;
        for (int i = 0; i < 100; ++i, t += 0.02) a.step(3.7, t, true);
        for (int i = 0; i < 400; ++i, t += 0.02) a.step(3.7, t, false);
        check(a.away && a.episodes == 1 && a.shopCalls == 100 && a.distanceCalls == 500, "F209: the screen closed with the camera still 3.7 m out: the distance test keeps the episode open");
        double backAt = -1.0, nearAt = t;
        for (int i = 0; i < 200; ++i, t += 0.02)
            if (a.step(0.3, t, false) == f107::Away::kBack) backAt = t;
        check(backAt >= nearAt + f107::kReturnHoldS - 1e-9 && !a.away, "F209: ... and the camera's return ends it after the hold");
    }
    {   // normal play unchanged: the same distances with the shop input absent, false, or true-then-never give what F208/F208b gave; the distance test is the only reason
        const double seq[] = {0.2, 0.2, 3367.4, 3367.6, 139.2, 0.19, 0.19, 3.7, 0.46, 0.46, 3.6, 0.5, 0.2, 710.5, 0.3, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2};
        f107::Away dflt, explicitOff;
        bool same = true;
        double t = 0.0;
        for (int round = 0; round < 12; ++round)
            for (const double d : seq) {
                t += 0.4;
                const f107::Away::Edge e1 = dflt.step(d, t);
                const f107::Away::Edge e2 = explicitOff.step(d, t, false);
                same = same && e1 == e2 && dflt.away == explicitOff.away && dflt.calls == explicitOff.calls && dflt.farCalls == explicitOff.farCalls && dflt.episodes == explicitOff.episodes &&
                       dflt.alternations == explicitOff.alternations && dflt.rejoins == explicitOff.rejoins && dflt.holdS == explicitOff.holdS;
            }
        check(same && dflt.shopCalls == 0 && dflt.episodes > 1 && !dflt.beganByShop && dflt.beganByDistance, "F209: with no buy screen open F107 behaves exactly as before: the shop input defaults off, and the distance test is the only reason an episode begins");
        f107::Away fresh;
        for (int i = 0; i < 10; ++i) fresh.step(0.2, i * 0.02, false);
        const f107::Away::Edge e = fresh.step(0.5, 1.0, false);
        check(e == f107::Away::kNone && !fresh.away && fresh.episodes == 0, "F209: the near camera 46 cm over the neck with no buy screen open is the commander's own: no episode");
        f107::Away carrier;
        const f107::Away::Edge left = carrier.step(3367.4, 0.0, false);
        check(left == f107::Away::kLeft && carrier.beganByDistance && !carrier.beganByShop && carrier.distanceCalls == 1 && carrier.shopCalls == 0,
              "F209: the carrier management screen (3367 m) is still away by the distance test alone");
        f107::Away nan;
        const f107::Away::Edge leftNan = nan.step(std::numeric_limits<double>::quiet_NaN(), 0.0, false);
        check(leftNan == f107::Away::kLeft && nan.notNumber == 1 && nan.beganByDistance, "F209: a camera that is not a number is still away by the distance test");
    }
    // ---- 5. the lines ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char line[512];
        shop::Event e;
        e.kind = shop::kShow;
        e.cls = shop::kSuit;
        e.before = 0;
        e.after = 1;
        e.tid = 7;
        e.thisLow = 0x1A2B3C4D;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): SuitVendorInteraction Show: +0x79 0 -> 1 (this 0x1A2B3C4D, thread 7)") == 0, "F209: a buy screen's Show line says the class, +0x79 before and after, the object and the thread");
        e.kind = shop::kClose;
        e.cls = shop::kWeapons;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): WeaponsVendorInteraction Close: +0x79 1 -> 0 (this 0x1A2B3C4D, thread 7)") == 0, "F209: a Close line likewise");
        e = shop::Event();
        e.kind = shop::kState;
        e.tid = 9;
        std::snprintf(e.from, sizeof(e.from), "%s", "OnFoot");
        std::snprintf(e.to, sizeof(e.to), "%s", "Suit Vendor");
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): SuitUIStateMachine: \"OnFoot\" -> \"Suit Vendor\" (thread 9)") == 0, "F209: a SuitUIStateMachine state change says from, to and the thread");
        e = shop::Event();
        e.kind = shop::kKioskStart;
        e.cls = shop::kKiosk;
        e.before = 0;
        e.after = 1;
        e.tid = 4;
        e.thisLow = 0x3C4D5E6F;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): ArmouryVendorActivity started: +0x1F8 0 -> 1 (this 0x3C4D5E6F, thread 4)") == 0, "F209: the kiosk's start line says +0x1F8 before and after");
        e.kind = shop::kKioskStop;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): ArmouryVendorActivity stopped: +0x1F8 1 -> 0 (this 0x3C4D5E6F, thread 4)") == 0, "F209: ... and its stop line");
        e.kind = shop::kShow;
        e.cls = shop::kSuit;
        e.before = -1;
        e.after = -1;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strstr(line, "+0x79 -1 -> -1") != nullptr, "F209: an unreadable byte reads -1 in the line");
        shop::Signal s;
        s.open[shop::kSuit] = true;
        shop::formatSignal(line, sizeof(line), s, true, 3);
        check(std::strcmp(line, "shop screens (F209): the shop signal is ON: F107 holds the camera away (suit open, weapons closed, kiosk not running, livery closed; change 3)") == 0, "F209: the signal's ON line says which screen and the change count (F228: and the Livery screens)");
        s = shop::Signal();
        s.kiosk = true;
        shop::formatSignal(line, sizeof(line), s, false, 4);
        check(std::strcmp(line, "shop screens (F209): the shop signal is OFF (suit closed, weapons closed, kiosk running, livery closed; change 4)") == 0, "F209: the OFF line likewise");
        s = shop::Signal();
        s.livery[1] = true;
        shop::formatSignal(line, sizeof(line), s, true, 5);
        check(std::strcmp(line, "shop screens (F209): the shop signal is ON: F107 holds the camera away (suit closed, weapons closed, kiosk not running, livery weapon open; change 5)") == 0, "F228: the ON line says which Livery screen");
        e = shop::Event();
        e.kind = shop::kShow;
        e.cls = shop::kLivery;
        e.before = 0;
        e.after = 1;
        e.tid = 7;
        e.thisLow = 0x4D5E6F70;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): HumanoidLiveryActivity Show: +0x79 0 -> 1 (this 0x4D5E6F70, thread 7)") == 0, "F228: a Livery screen's Show line names the class");
        e.kind = shop::kClose;
        e.cls = shop::kWeaponLivery;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): HumanoidWeaponLiveryActivity Close: +0x79 1 -> 0 (this 0x4D5E6F70, thread 7)") == 0, "F228: the weapon Livery's Close line names the class");
        shop::formatLiveryOpened(line, sizeof(line), shop::kLivery, true);
        check(std::strcmp(line, "shop screens (F209): HumanoidLiveryActivity opened: F107 holds the camera away, the view is the game's own (experimental.native_on_foot_livery_game_view is on)") == 0,
              "F228: a Livery screen opening with the key on says it holds the camera away");
        shop::formatLiveryOpened(line, sizeof(line), shop::kWeaponLivery, false);
        check(std::strcmp(line, "shop screens (F209): HumanoidWeaponLiveryActivity opened: only watched, the camera is not held away (experimental.native_on_foot_livery_game_view is off)") == 0,
              "F228: ... with the key off it says it is only watched");
        shop::formatForgotten(line, sizeof(line), shop::kSuit, shop::Verdict::kStaleVtable, 0x7FF600001234ull, 0x7FF605255D20ull, 0x1A2B3C4D);
        check(std::strstr(line, "the remembered SuitVendorInteraction (this 0x1A2B3C4D) is forgotten: its vtable is no longer the class's (vtable 0x7FF600001234, the class's 0x7FF605255D20)") != nullptr,
              "F209: the forgotten line says the class, the object and both vtables");
        shop::formatSecond(line, sizeof(line), 1, -1, 0, -1, -1, "Suit Vendor", true, "away (episode 1, 90 calls refused, 90 with a buy screen open)");
        check(std::strcmp(line, "shop screens (F209): 1 s: suit +0x79 1, weapons +0x79 none, kiosk +0x1F8 0, livery +0x79 none, weapon livery +0x79 none, SuitUIStateMachine \"Suit Vendor\", signal on; F107: away (episode 1, 90 calls refused, 90 with a buy screen open)") == 0,
              "F209: the once-a-second line says the remembered bytes, the state, the kiosk byte, the signal and F107's state (F228: and the Livery bytes)");
        shop::formatSecond(line, sizeof(line), -1, -1, -1, 1, 0, "", true, "away");
        check(std::strstr(line, "livery +0x79 1, weapon livery +0x79 0, SuitUIStateMachine \"unknown\", signal on") != nullptr, "F228: the Livery bytes read in the once-a-second line");
        shop::formatSecond(line, sizeof(line), -1, -1, -1, -1, -1, "", false, "not away");
        check(std::strstr(line, "SuitUIStateMachine \"unknown\", signal off; F107: not away") != nullptr && std::strstr(line, "suit +0x79 none") != nullptr, "F209: no state yet reads unknown, no object reads none");
    }
    // ---- 6. the cap and the pacer; what is "set" -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        shop::LineCap cap;
        uint32_t summary = 99;
        int written = 0;
        for (int i = 0; i < 100; ++i)
            if (cap.allow(1000 + static_cast<uint64_t>(i), &summary)) ++written;
        check(written == static_cast<int>(shop::kLinesPer10s) && cap.dropped == 100 - shop::kLinesPer10s, "F209: a flapping screen writes at most 40 event lines in ten seconds; the rest are counted");
        const bool next = cap.allow(1000 + 10000, &summary);
        check(next && summary == 100 - shop::kLinesPer10s, "F209: the next window says how many the last one dropped, once");
        const bool again = cap.allow(1000 + 10001, &summary);
        check(again && summary == 0, "F209: ... and not again");
        shop::LineCap quiet;
        bool all = true;
        for (int w = 0; w < 5; ++w)
            for (int i = 0; i < 10; ++i) all = all && quiet.allow(5000 + static_cast<uint64_t>(w) * 10000 + static_cast<uint64_t>(i) * 100, &summary) && summary == 0;
        check(all, "F209: ordinary shop traffic (a handful of lines a window) is never capped");
        shop::Pacer p;
        check(p.due(5000) && !p.due(5500) && !p.due(5999) && p.due(6000) && !p.due(6001) && p.due(7001) && p.due(0) && !p.due(500), "F209: the once-a-second line is due on the first look, then every 1000 ms (a clock that went back is due once)");
        check(shop::stateIsVendor("Suit Vendor") && shop::stateIsVendor("Weapons Upgrade Vendor") && shop::stateIsVendor("Armoury Vendor") && !shop::stateIsVendor("OnFoot") && !shop::stateIsVendor("") &&
                  !shop::stateIsVendor(nullptr) && !shop::stateIsVendor("Cockpit"),
              "F209: a vendor state is one whose name has Vendor in it; OnFoot, an empty state and a state the build does not name are not");
        check(shop::anySet(true, false, "OnFoot") && shop::anySet(false, true, "") && shop::anySet(false, false, "Suit Vendor") && !shop::anySet(false, false, "OnFoot") && !shop::anySet(false, false, ""),
              "F209: the once-a-second line runs while a buy screen is open, the kiosk runs, or the machine is in a vendor state, and is silent otherwise");
        check(shop::anySet(false, false, "OnFoot", true) && shop::anySet(false, false, "", true) && !shop::anySet(false, false, "OnFoot", false) && !shop::anySet(false, false, "OnFoot"),
              "F228: the once-a-second line also runs while a Livery screen is open (held or only watched)");
        check(shop::isSuitUiStateMachine("SuitUIStateMachine") && !shop::isSuitUiStateMachine("SuitUIStateMachineX") && !shop::isSuitUiStateMachine("SuitUIState") && !shop::isSuitUiStateMachine("") &&
                  !shop::isSuitUiStateMachine(nullptr) && !shop::isSuitUiStateMachine("OnFoot"),
              "F209: only the handle named exactly SuitUIStateMachine is recorded");
    }
    // ---- 7. the wiring, read as text -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"), cpp = slurp("src/d3d11/native_on_foot.cpp");
        check(countOf(hooks, "shop::kShowRva") >= 1 && countOf(hooks, "shop::kCloseRva") >= 1 && countOf(hooks, "shop::kSetStateRva") >= 1 && countOf(hooks, "shop::kKioskStartRva") >= 1 &&
                  countOf(hooks, "shop::kKioskStopRva") >= 1 && countOf(hooks, "case kHkShow: return reinterpret_cast<const void*>(&shopShowHooked)") == 1 &&
                  countOf(hooks, "case kHkKioskStop: return reinterpret_cast<const void*>(&shopKioskStopHooked)") == 1,
              "F209: the five hooks are in the hook table with their addresses and callbacks");
        check(hooks.find("tryShopHooks(sink);") != std::string::npos && hooks.find("shopFrameTick(sink);") != std::string::npos && hooks.find("shopFrameTick(sink);") < hooks.find("    updateGates();\n}\navh::Sink g_frameSink"),
              "F209: the frame boundary tries the shop hooks (while the native key is on) and re-reads the remembered objects every frame, before the gates are set");
        check(cpp.find("g_f107Away.step(eyeNeckM, static_cast<double>(f30QpcUs(in.qpc)) * 1e-6, nativeOnFootShopOpen())") != std::string::npos,
              "F209: the Animate post-hook gives F107 the shop signal with every call");
        // F228: the key is read live on the frame thread (default on), logged on each change as TEMPORARY, handed to the hooks, dumped with the recording's keys, and shipped on with its menu row
        check(has_(cpp, "cfg.getString(\"experimental.native_on_foot_livery_game_view\", \"on\") != \"off\"") && has_(cpp, "nativeOnFootSetLiveryHold(on);") && has_(cpp, "if (first || on != g_f228KeyLast)") &&
                  has_(cpp, "native on foot livery screens (F228, TEMPORARY): experimental.native_on_foot_livery_game_view is %s") && has_(cpp, "str(\"experimental.native_on_foot_livery_game_view\""),
              "F228: the key is read live on the frame thread, default on, logged on a change as TEMPORARY, set into the hooks and dumped");
        const std::string ini = slurp("edvr.ini");
        check(has_(ini, "# ui: Livery uses game view | choices on, off | recommended on | live\nnative_on_foot_livery_game_view = on\n") && has_(ini, "# TEMPORARY (F228): Livery uses game view"),
              "F228: the shipped key is on, TEMPORARY, with its ui line");
        check(has_(hooks, "return shop::screenOfVtable(vtable, base);") && has_(hooks, "s.holds(g_liveryHold.load(std::memory_order_relaxed))") &&
                  has_(hooks, "bool nativeOnFootShopOpen() noexcept { return shopBuyScreenOpen() || (g_liveryHold.load(std::memory_order_relaxed) && nativeOnFootLiveryOpen()); }") &&
                  has_(hooks, "std::atomic<int8_t> g_shopByte[shop::kClassCount] = {{-1}, {-1}, {-1}, {-1}, {-1}, {-1}};"),
              "F228: the Show/Close filter takes the Livery vtables, the frame thread's signal and F107's input hold a Livery screen only with the key on, and every per-class array is sized by kClassCount");
        // every shop callback forwards to the original: the original's result is returned unchanged (nothing is written to the game)
        const size_t at = hooks.find("// ---- F209: the Pioneer Supplies buy screens");
        const size_t end = hooks.find("// ---- hiding the head parts", at);
        const std::string block = at != std::string::npos && end != std::string::npos ? hooks.substr(at, end - at) : std::string();
        check(!block.empty() && countOf(block, "forward(a, b, c, d)") >= 5 && block.find("VirtualProtect") == std::string::npos && block.find("WriteProcessMemory") == std::string::npos &&
                  block.find("Log::get()") == std::string::npos && block.find("= new ") == std::string::npos && block.find("std::vector") == std::string::npos && block.find("std::string") == std::string::npos &&
                  block.find("malloc(") == std::string::npos && block.find("lock_guard") == std::string::npos && block.find("std::mutex") == std::string::npos,
              "F209: the hook threads' code forwards every call and writes nothing to the game, takes no lock, allocates nothing and logs nothing");
    }
    // ---- 8. F229: the Livery camera instrument (read-only): the client offsets, the field reader's formatting, the setup call's lines --------------------------------------------------------
    {
        const uint64_t base = 0x7FF600000000ull;
        check(shop::kOutfittingVtableRva == 0x524D908 && shop::kSetupRva == 0x12714C0 && shop::kSetupPrologueBytes == 16 && shop::kSetupPrologue[0] == 0x4C && shop::kSetupPrologue[1] == 0x8B &&
                  shop::kSetupPrologue[2] == 0xDC && shop::kSetupPrologue[3] == 0x55 && shop::kSetupPrologue[4] == 0x41 && shop::kSetupPrologue[5] == 0x54 && shop::kSetupPrologue[10] == 0xAB &&
                  shop::kSetupPrologue[11] == 0x98 && shop::kSetupPrologue[12] == 0xF8 && shop::kSetupPrologue[15] == 0x48 && shop::kOffMode == 0x398 && shop::kOffKind == 0x39C &&
                  shop::kOffStep == 0x570 && shop::kOffDist == 0x580 && shop::kOffMax == 0x590 && shop::kOffMin == 0x5A0 && shop::kOffRetry == 0x5B0 && shop::kOffPose == 0x5E0 && shop::kClientInterface == 0x58,
              "F229: the setup's address and 16 prologue bytes, the loadout browser's vtable and the camera fields' offsets are the RE note's");
        check(shop::clientOffset(shop::kSuit) == 0x448 && shop::clientOffset(shop::kWeapons) == 0x448 && shop::clientOffset(shop::kLivery) == 0x680 && shop::clientOffset(shop::kWeaponLivery) == 0x630 &&
                  shop::clientOffset(shop::kOutfitting) == 0xC0 && shop::clientOffset(shop::kKiosk) == 0 && shop::clientOffset(shop::kNotOurs) == 0,
              "F229: each class's preview client lies where its constructor's `lea rcx,[object+X]` before the call to 0x126B670 puts it (the kiosk has none)");
        const uint64_t out = base + shop::kOutfittingVtableRva;
        check(shop::screenOfVtable(out, base) == shop::kOutfitting && shop::buyScreenOfVtable(out, base) == shop::kNotOurs && shop::vtableRva(shop::kOutfitting) == shop::kOutfittingVtableRva &&
                  std::strcmp(shop::className(shop::kOutfitting), "HumanoidOutfittingInteraction") == 0 && !shop::isLivery(shop::kOutfitting) && shop::screenOfVtable(out, base + 0x1000) == shop::kNotOurs,
              "F229: the loadout browser joins the Show/Close filter as a class of its own (not a buy screen, not a Livery screen)");
        {   // the loadout browser has no input into the signal: signalOf takes five verdicts and none is its own
            const shop::Signal s = shop::signalOf(shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone);
            check(!s.holds(true) && !s.holds(false) && !s.liveryOpen() && !s.buyScreenOpen(), "F229: with no buy screen, kiosk or Livery screen open the signal is off (the loadout browser has no input into it)");
        }
        // the reader: the mode and kind words, four floats, the retry byte and the 64-byte pose in, a CamFields out
        uint8_t mk[8], st4[4], di4[4], mx4[4], mn4[4], pose[64];
        const int32_t mode = 3, kind = 1;
        std::memcpy(mk, &mode, 4);
        std::memcpy(mk + 4, &kind, 4);
        const float step = 0.25f, dist = 4.0f, maxD = 9.0f, minD = 0.0f;
        std::memcpy(st4, &step, 4);
        std::memcpy(di4, &dist, 4);
        std::memcpy(mx4, &maxD, 4);
        std::memcpy(mn4, &minD, 4);
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = static_cast<float>(i) * 0.5f - 1.0f;
        m[12] = 3.0f;
        m[13] = 4.0f;
        m[14] = 0.0f;    // the last row's translation: length 5
        m[3] = 0.0f;
        m[7] = 0.0f;
        m[11] = 12.0f;   // the last column's: length 12
        std::memcpy(pose, m, 64);
        const shop::CamFields f = shop::camFromBytes(mk, st4, di4, mx4, mn4, 1, pose);
        check(f.mode == 3 && f.kind == 1 && f.step == 0.25f && f.dist == 4.0f && f.maxDist == 9.0f && f.minDist == 0.0f && f.retry == 1 && f.pose[0] == -1.0f && f.pose[15] == 6.5f && f.pose[12] == 3.0f &&
                  shop::camFinite(f),
              "F229: the reader splits its blocks into the mode and kind, the zoom step, the current, maximum and minimum distances, the retry byte and the 16 pose floats, in order");
        char line[1100];
        const int n = shop::formatCamera(line, sizeof(line), shop::kLivery, 0x1A2B3C4Du, 0x1A2B42CDu, f);
        const std::string text = line;
        check(n > 0 && static_cast<size_t>(n) < sizeof(line) && text.rfind("livery camera (F229): HumanoidLiveryActivity 1 s: this 0x1A2B3C4D, client 0x1A2B42CD: ", 0) == 0 &&
                  has_(text, "mode 3, kind 1, step 0.2500, dist 4.0000, max 9.0000, min 0.0000, retry 1, pose rows [-1.0000 -0.5000 0.0000 0.0000]") &&
                  has_(text, "[3.0000 4.0000 0.0000 6.5000], translation length last row 5.0000, last column 12.0000") && std::count(text.begin(), text.end(), '[') == 4 && std::count(text.begin(), text.end(), ']') == 4,
              "F229: the once-a-second line names the class, the object and the client, the named fields (mode, kind, step, dist, max, min, retry), the pose as four rows and both translation lengths");
        shop::CamFields wild = f;
        wild.dist = std::numeric_limits<float>::quiet_NaN();
        wild.pose[5] = 3.0e30f;
        wild.pose[6] = -std::numeric_limits<float>::infinity();
        shop::formatCamera(line, sizeof(line), shop::kWeaponLivery, 1u, 2u, wild);
        const std::string wtext = line;
        check(!shop::camFinite(wild) && has_(wtext, "dist nan") && has_(wtext, "3.000e+30") && has_(wtext, "-inf") && wtext.size() < 1000,
              "F229: a value that is not a number reads 'nan' or 'inf', a huge one takes an exponent, and none of them swells the line");
        shop::formatCameraUnreadable(line, sizeof(line), shop::kOutfitting, 0x11u, 0xD1u, true);
        const std::string un = line;
        shop::formatCameraUnreadable(line, sizeof(line), shop::kOutfitting, 0x11u, 0xD1u, false);
        check(has_(un, "livery camera (F229): HumanoidOutfittingInteraction (this 0x00000011) client 0x000000D1: the camera fields could not be read") && has_(std::string(line), "hold a value that is not a number"),
              "F229: the one-time line says whether the client could not be read or holds a value that is not a number");
        // the setup call: where rcx lies
        int32_t delta = -1;
        check(shop::locateInObject(0x10680, 0x10000, &delta) && delta == 0x680 && shop::locateInObject(0x10000, 0x10000, &delta) && delta == 0 && !shop::locateInObject(0xFFFF, 0x10000, &delta) &&
                  !shop::locateInObject(0x10000 + shop::kSetupObjectSpan, 0x10000, &delta) && !shop::locateInObject(0x10680, 0, &delta),
              "F229: rcx is 'in' a watched screen's object from its start to 0x1000 past it, and in none when nothing is remembered");
        shop::SetupCall c;
        c.tid = 7;
        c.rcxLow = 0x1A2B42CDu;
        c.objLow = 0x1A2B4275u;
        c.r8Low = 0x00FF0010u;
        c.modeRead = true;
        c.mode = 3;
        c.cls = static_cast<int8_t>(shop::kLivery);
        c.delta = 0x680;
        c.qwordRead = true;
        c.qword = 3;
        shop::formatSetup(line, sizeof(line), c);
        const std::string setup = line;
        check(setup == "livery camera (F229): setup call 0x12714C0: rcx 0x1A2B42CD, object (rcx - 0x58) 0x1A2B4275, r8 0x00FF0010, mode 3, qword 0x3 (thread 7), the object is HumanoidLiveryActivity object +0x680, which is client +0x0, camera node not tested",
              "F229: the setup call's line names rcx, rcx - 0x58 as the object, r8, the mode int, the thread, and the object as the watched screen's client");
        c.cls = static_cast<int8_t>(shop::kNotOurs);
        c.modeRead = false;
        shop::formatSetup(line, sizeof(line), c);
        check(has_(std::string(line), "mode unreadable, qword 0x3 (thread 7), the object is in no watched screen's object"), "F229: an unreadable mode and an object in no watched screen say so");
        c.cls = static_cast<int8_t>(shop::kSuit);
        c.delta = 0x40;   // below the client
        shop::formatSetup(line, sizeof(line), c);
        check(has_(std::string(line), "SuitVendorInteraction object +0x40, which is client -0x408"), "F229: an object before the client reads as a negative offset from it");
        shop::formatCamCapped(line, sizeof(line), "camera", 12);
        const std::string capped = line;
        shop::formatSetupLost(line, sizeof(line), 3);
        check(capped == "livery camera (F229): 12 camera lines in the last 10 s were not written (at most 40 in any ten seconds)" &&
                  std::string(line) == "livery camera (F229): 3 setup call(s) were not recorded (the ring was full or a second hook thread was in it)",
              "F229: the cap's note and the lost-calls note name what was dropped");
        // the wiring, read as text
        const std::string hooks = slurp("src/d3d11/native_on_foot_hooks.cpp");
        const size_t at = hooks.find("avh::Ring<shop::SetupCall, 32> g_setupRing;");
        const size_t end = hooks.find("// ---- hiding the head parts", at);
        const std::string block = at != std::string::npos && end != std::string::npos ? hooks.substr(at, end - at) : std::string();
        check(!block.empty() && countOf(block, "return forward(a, b, c, d);") == 1 && block.find("VirtualProtect") == std::string::npos && block.find("WriteProcessMemory") == std::string::npos &&
                  block.find("Log::get()") == std::string::npos && block.find("std::string") == std::string::npos && block.find("lock_guard") == std::string::npos && block.find("= new ") == std::string::npos,
              "F229/F230: the setup and Update callbacks forward their calls (the Update always, unchanged), map no memory, write no process memory, take no lock, allocate nothing and log nothing");
        check(has_(hooks, "case kHkLiveryCam: return reinterpret_cast<const void*>(&livCamSetupHooked);") && has_(hooks, "{\"livery-camera-setup\"") && has_(hooks, "shop::kSetupRva,") &&
                  has_(hooks, "liveryCameraTick(sink);   // F229") && has_(hooks, "g_gate[kHkLiveryCam].store(armed(kHkLiveryCam) && capture ? 1u : 0u, std::memory_order_release);") &&
                  has_(hooks, "if (g_hooks[kHkLiveryCam].state == kHookStateNotTried && liveryCamWanted()) tryHook(kHkLiveryCam, sink);"),
              "F229: the setup hook is in the hook table, tried at the frame boundary while the native key is on, gated with it, and the camera fields are read every frame");
    }
    // ---- 9. F230: the Livery camera repair (TEMPORARY key): constants, the decision core's gates, the lines ---------------------------------------------------------------------------------------
    {
        check(shop::kUpdateRva == 0x1272780 && shop::kNodeValidRva == 0x3175B40 && shop::kUpdatePrologueBytes == 16 && shop::kUpdatePrologue[0] == 0x4C && shop::kUpdatePrologue[1] == 0x8B &&
                  shop::kUpdatePrologue[2] == 0xDC && shop::kUpdatePrologue[3] == 0x49 && shop::kUpdatePrologue[4] == 0x89 && shop::kUpdatePrologue[5] == 0x5B && shop::kUpdatePrologue[6] == 0x18 &&
                  shop::kUpdatePrologue[7] == 0x56 && shop::kUpdatePrologue[8] == 0x48 && shop::kUpdatePrologue[11] == 0x90 && shop::kUpdatePrologue[15] == 0x41 && shop::kOffNode == 0x478 &&
                  shop::kOffSetupHandle == 0x390 && shop::kRepairMinMs == 500 && shop::kRepairMaxAttempts == 5 && std::strcmp(shop::kRepairKey, "experimental.native_on_foot_livery_camera_repair") == 0,
              "F230: the Update's address and 16 prologue bytes (seven stolen: mov r11,rsp; mov [r11+18h],rbx), the node test's address, the node and the setup handle's offsets, 0.5 s, five attempts, the key's name");
        check(shop::isLiverySetupMode(1, true) && shop::isLiverySetupMode(4, true) && shop::isLiverySetupMode(0x100000001ull, true) && !shop::isLiverySetupMode(0, true) && !shop::isLiverySetupMode(2, true) &&
                  !shop::isLiverySetupMode(3, true) && !shop::isLiverySetupMode(1, false) && !shop::isLiverySetupMode(4, false),
              "F230: a Livery setup is a low dword of 1 (suit) or 4 (weapon) while a Livery screen reads open; mode 0, 2 and 3, or no Livery screen open, are not");
        using V = shop::RepairVerdict;
        shop::RepairIn in;
        in.keyOn = true;
        in.remembered = true;
        in.liveryOpen = true;
        in.sameThread = true;
        in.nowMs = 10000;
        in.lastSetupMs = 9000;
        in.attempts = 0;
        check(shop::repairDecide(in) == V::kTestNode, "F230: every condition holds: the camera node is tested (and repaired when invalid)");
        shop::RepairIn x = in;
        x.keyOn = false;
        check(shop::repairDecide(x) == V::kKeyOff, "F230: the key off: nothing is run");
        x = in;
        x.remembered = false;
        check(shop::repairDecide(x) == V::kNotRemembered, "F230: a client whose Livery setup was not remembered (the loadout browser, a buy screen) is left alone");
        x = in;
        x.liveryOpen = false;
        check(shop::repairDecide(x) == V::kScreenClosed, "F230: no Livery screen reads open: nothing is run");
        x = in;
        x.sameThread = false;
        check(shop::repairDecide(x) == V::kOtherThread, "F230: another thread than the one that ran the setup: nothing is run");
        x = in;
        x.lastSetupMs = 9501;
        check(shop::repairDecide(x) == V::kTooSoon, "F230: under 0.5 s after the last setup (the game's or ours): too soon");
        x.lastSetupMs = 9500;
        check(shop::repairDecide(x) == V::kTestNode, "F230: exactly 0.5 s after it: due");
        x.lastSetupMs = 10500;   // a clock that went back
        check(shop::repairDecide(x) == V::kTooSoon, "F230: a clock that went back is too soon, not due");
        x = in;
        x.attempts = 4;
        check(shop::repairDecide(x) == V::kTestNode, "F230: the fifth attempt is allowed");
        x.attempts = 5;
        check(shop::repairDecide(x) == V::kExhausted, "F230: five attempts for one open are the most");
        x = in;
        x.keyOn = false;
        x.remembered = false;
        check(shop::repairDecide(x) == V::kKeyOff, "F230: the key is the first gate");
        char line[1100];
        shop::RepairEvent e;
        e.tid = 7;
        e.attempt = 2;
        e.clientLow = 0x4D5E75F0u;
        e.qword = 0x100000001ull;
        e.after = 1;
        shop::formatRepair(line, sizeof(line), e);
        check(std::string(line) == "livery camera repair (F230): attempt 2 of 5: the setup was run again by EDVR (client 0x4D5E75F0, mode 1, qword 0x100000001, thread 7), camera node INVALID before, valid after",
              "F230: the retry line names the attempt, the client, the mode and qword handed to the setup, the thread, the node before and after");
        e.after = 0;
        shop::formatRepair(line, sizeof(line), e);
        const std::string stillBad = line;
        e.faulted = true;
        shop::formatRepair(line, sizeof(line), e);
        check(has_(stillBad, "INVALID before, INVALID after") && has_(std::string(line), "the call faulted"), "F230: a setup that left the node invalid, and one that faulted, say so");
        shop::formatRepairKey(line, sizeof(line), true);
        const std::string keyOn = line;
        shop::formatRepairKey(line, sizeof(line), false);
        check(keyOn.rfind("native on foot livery camera (F230, TEMPORARY): experimental.native_on_foot_livery_camera_repair is on -- ", 0) == 0 &&
                  has_(std::string(line), "experimental.native_on_foot_livery_camera_repair is off -- the Livery preview's camera node is only logged (F229); EDVR runs nothing"),
              "F230: the key's line says what each value does");
        shop::SetupCall c;
        c.tid = 7;
        c.rcxLow = 0x1A2B42CDu;
        c.objLow = 0x1A2B4275u;
        c.r8Low = 0x00FF0010u;
        c.modeRead = true;
        c.mode = 1;
        c.qwordRead = true;
        c.qword = 0x100000001ull;
        c.cls = static_cast<int8_t>(shop::kLivery);
        c.delta = 0x680;
        c.node = 0;
        shop::formatSetup(line, sizeof(line), c);
        const std::string setupBad = line;
        c.node = 1;
        c.ours = true;
        shop::formatSetup(line, sizeof(line), c);
        const std::string setupOurs = line;
        c.node = -1;
        c.ours = false;
        c.qwordRead = false;
        shop::formatSetup(line, sizeof(line), c);
        check(has_(setupBad, "mode 1, qword 0x100000001 (thread 7), the object is HumanoidLiveryActivity object +0x680, which is client +0x0, camera node INVALID") && !has_(setupBad, "EDVR's repair call") &&
                  has_(setupOurs, "camera node valid (EDVR's repair call)") && has_(std::string(line), "qword unreadable") && has_(std::string(line), "camera node not tested"),
              "F229/F230: the setup line carries the qword, the camera node's state (valid, INVALID or not tested) and marks EDVR's own call");
        // the wiring, read as text
        const std::string hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"), cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini");
        check(has_(hooks, "case kHkLiveryUpdate: return reinterpret_cast<const void*>(&livUpdateHooked);") && has_(hooks, "{\"livery-camera-update\"") && has_(hooks, "shop::kUpdateRva,") &&
                  has_(hooks, "g_gate[kHkLiveryUpdate].store(armed(kHkLiveryUpdate) && capture ? 1u : 0u, std::memory_order_release);") &&
                  has_(hooks, "tryHook(kHkLiveryUpdate, sink);") && has_(hooks, "if (g_repairKey.load(std::memory_order_relaxed) && nativeOnFootLiveryOpen()) livRepairStep(reinterpret_cast<uintptr_t>(a));") &&
                  has_(hooks, "return forward(a, b, c, d);\n}\n\n// ---- hiding the head parts") && has_(hooks, "if (show && shop::isLivery(cls))\n        for (SetupMemo& m : g_memo) m.attempts.store(0"),
              "F230: the Update hook is in the table, gated and tried with the others, its fast path is the key and the Livery reading, it always forwards, and a Livery Show resets the attempts");
        check(has_(cpp, "cfg.getString(\"experimental.native_on_foot_livery_camera_repair\", \"on\") != \"off\"") && has_(cpp, "nativeOnFootSetLiveryRepair(on);") && has_(cpp, "shop::formatRepairKey(line, sizeof(line), on);") &&
                  has_(cpp, "str(\"experimental.native_on_foot_livery_camera_repair\""),
              "F230: the key is read live on the frame thread, default on, logged on a change, set into the hooks and dumped");
        check(has_(ini, "# ui: Livery camera repair | choices on, off | recommended on | live\nnative_on_foot_livery_camera_repair = on\n") && has_(ini, "# TEMPORARY (F230): Livery camera repair") &&
                  has_(ini, "The preview stands beside the panel instead of around you.") && ini.find("native_on_foot_livery_camera_repair = on") > ini.find("native_on_foot_livery_game_view = on"),
              "F230: the shipped key is on, TEMPORARY, with its ui line after the F228 row and the plain sentence of what the user gets");
    }
    // ---- 10. F231: the preview camera patch (TEMPORARY key): the bytes, when it is wanted, the step against synthetic bytes, the lines ---------------------------------------------------------------
    {
        namespace pc = edvr::previewcam;
        check(pc::kPatchRva == 0x126FE70 && pc::kVerifyRva == 0x126FE6C && pc::kVerifyLen == 21 && pc::kPatchLen == 3 && (pc::kPatchRva & 7u) == 0 && pc::kSetCameraRva == 0x1D7BEC0 && pc::kReleaseRva == 0x1D7C1E0 &&
                  pc::kPreviewCallerRva == 0x126FE81 && std::strcmp(pc::kKey, "experimental.native_on_foot_preview_camera") == 0,
              "F231: the patch's address 0x126FE70 (an aligned qword), the 21 verified bytes from 0x126FE6C, SetCamera 0x1D7BEC0, Release 0x1D7C1E0, the preview client's return address 0x126FE81, the key's name");
        bool onlyThree = true;
        for (size_t i = 0; i < pc::kVerifyLen; ++i) {
            const bool inPatch = i >= 4 && i < 7;
            onlyThree = onlyThree && (inPatch ? pc::kVerifyFrom[i] != pc::kVerifyTo[i] : pc::kVerifyFrom[i] == pc::kVerifyTo[i]);
        }
        check(onlyThree && pc::kVerifyFrom[4] == 0x45 && pc::kVerifyFrom[5] == 0x33 && pc::kVerifyFrom[6] == 0xC0 && pc::kVerifyTo[4] == 0x41 && pc::kVerifyTo[5] == 0xB0 && pc::kVerifyTo[6] == 0x01 &&
                  pc::kFrom3[0] == 0x45 && pc::kTo3[2] == 0x01 && pc::kVerifyFrom[20] == 0x30,
              "F231: xor r8d,r8d (45 33 C0) becomes mov r8b,1 (41 B0 01) and nothing else of the 21 bytes differs");
        check(pc::kSetCameraPrologueBytes == 20 && pc::kSetCameraPrologue[0] == 0x48 && pc::kSetCameraPrologue[4] == 0x18 && pc::kSetCameraPrologue[13] == 0x48 && pc::kSetCameraPrologue[19] == 0x50 &&
                  pc::kReleasePrologueBytes == 13 && pc::kReleasePrologue[0] == 0x48 && pc::kReleasePrologue[4] == 0x10 && pc::kReleasePrologue[12] == 0xF9,
              "F231: SetCamera's 20 and Release's 13 prologue bytes are the exe's (five bytes stolen from each: a whole first instruction)");
        // when it is wanted: the key, native on foot, the HUD override held and the session, all four
        bool onlyAll = true, reasonsRight = true;
        for (int m = 0; m < 16; ++m) {
            const bool k = (m & 1) != 0, n = (m & 2) != 0, h = (m & 4) != 0, s = (m & 8) != 0;
            const char* why = pc::notWantedWhy(k, n, h, s);
            onlyAll = onlyAll && ((why == nullptr) == (k && n && h && s));
            if (!k) reasonsRight = reasonsRight && why && std::strcmp(why, "experimental.native_on_foot_preview_camera is off") == 0;
            else if (!n) reasonsRight = reasonsRight && why && std::strcmp(why, "native stereo on foot ended") == 0;
            else if (!h) reasonsRight = reasonsRight && why && std::strcmp(why, "the first-person HUD override is off") == 0;
            else if (!s) reasonsRight = reasonsRight && why && std::strcmp(why, "the game session ended") == 0;
        }
        check(onlyAll && reasonsRight, "F231: of the sixteen combinations of the key, native on foot, the HUD override and the session only all four want the patch; each other says which one is missing, in that order");
        // the step against a synthetic image of the 21 bytes and the qword they sit in
        struct Fake {
            uint8_t mem[32] = {};
            bool readable = true, casWorks = true;
            int casCalls = 0, interfere = 0;   // interfere: the first n compare-exchanges find the qword changed under them
            uint64_t lastExpected = 0, lastDesired = 0;
            void seed(const uint8_t* bytes) { std::memset(mem, 0xEE, sizeof(mem)); std::memcpy(mem, bytes, pc::kVerifyLen); const uint8_t extra[4] = {0x11, 0x22, 0x33, 0x44}; std::memcpy(mem + 21, extra, 4); }
            bool read(uintptr_t rva, uint8_t* out, size_t n) { if (!readable) return false; std::memcpy(out, mem + (rva - pc::kVerifyRva), n); return true; }
            bool cas(uintptr_t rva, uint64_t expected, uint64_t desired) {
                ++casCalls;
                lastExpected = expected;
                lastDesired = desired;
                if (!casWorks || interfere > 0) { if (interfere > 0) --interfere; return false; }
                uint64_t cur;
                std::memcpy(&cur, mem + (rva - pc::kVerifyRva), 8);
                if (cur != expected) return false;
                std::memcpy(mem + (rva - pc::kVerifyRva), &desired, 8);
                return true;
            }
        };
        auto run = [](Fake& f, pc::Patch& p, bool want, bool id, uint8_t* seen) {
            return pc::step(p, want, id, [&](uintptr_t r, uint8_t* o, size_t n) { return f.read(r, o, n); }, [&](uintptr_t r, uint64_t e, uint64_t d) { return f.cas(r, e, d); }, seen);
        };
        uint8_t seen[pc::kVerifyLen] = {};
        {
            Fake f;
            f.seed(pc::kVerifyFrom);
            pc::Patch p;
            check(run(f, p, false, true, seen) == pc::Event::kNone && !p.applied && f.casCalls == 0, "F231: not wanted and not applied: nothing is read or written");
            const pc::Event e = run(f, p, true, true, seen);
            check(e == pc::Event::kApplied && p.applied && f.casCalls == 1 && std::memcmp(f.mem, pc::kVerifyTo, pc::kVerifyLen) == 0 && f.mem[21] == 0x11 && f.mem[24] == 0x44,
                  "F231: wanted with the 21 bytes as the exe has them: applied in ONE compare-exchange, the 21 bytes now read mov r8b,1 and the bytes after them are untouched");
            check((f.lastDesired & 0xFFFFFFull) == 0x01B041ull && ((f.lastDesired >> 24) == (f.lastExpected >> 24)),
                  "F231: the qword written is the qword read with only its first three bytes replaced (41 B0 01): the other five are carried through");
            check(run(f, p, true, true, seen) == pc::Event::kNone && f.casCalls == 1, "F231: wanted and already applied: nothing more is written");
            f.mem[21] = 0x99;
            check(run(f, p, false, true, seen) == pc::Event::kRestored && !p.applied && std::memcmp(f.mem, pc::kVerifyFrom, pc::kVerifyLen) == 0 && f.casCalls == 2 && f.mem[21] == 0x99,
                  "F231: no longer wanted: the verified bytes still read as ours, so xor r8d,r8d is written back in one compare-exchange and a byte outside the three is not touched");
            check(run(f, p, false, true, seen) == pc::Event::kNone && f.casCalls == 2, "F231: and put back once only");
        }
        {   // every state combination from a clean image and from an applied one
            bool right = true;
            for (int m = 0; m < 16; ++m) {
                const bool want = pc::notWantedWhy((m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0) == nullptr;
                Fake f;
                f.seed(pc::kVerifyFrom);
                pc::Patch p;
                right = right && (run(f, p, want, true, seen) == (want ? pc::Event::kApplied : pc::Event::kNone)) && p.applied == want;
                Fake g;
                g.seed(pc::kVerifyFrom);
                pc::Patch q;
                run(g, q, true, true, seen);
                right = right && (run(g, q, want, true, seen) == (want ? pc::Event::kNone : pc::Event::kRestored)) && q.applied == want &&
                        std::memcmp(g.mem, want ? pc::kVerifyTo : pc::kVerifyFrom, pc::kVerifyLen) == 0;
            }
            check(right, "F231: for every combination of the key, native on foot, the HUD override and the session: applied exactly when all four hold, and an applied patch is restored exactly when one stops holding");
        }
        {   // refusals
            Fake f;
            f.seed(pc::kVerifyFrom);
            pc::Patch p;
            check(run(f, p, true, false, seen) == pc::Event::kRefusedIdentity && !p.applied && f.casCalls == 0, "F231: a build whose identity differs: refused, nothing read or written");
            f.readable = false;
            check(run(f, p, true, true, seen) == pc::Event::kRefusedUnreadable && !p.applied && f.casCalls == 0, "F231: bytes that cannot be read: refused");
            f.readable = true;
            f.mem[4] = 0x44;   // someone else's byte inside the instruction
            std::memset(seen, 0, sizeof(seen));
            check(run(f, p, true, true, seen) == pc::Event::kRefusedBytes && !p.applied && f.casCalls == 0 && seen[4] == 0x44, "F231: a byte that is not the exe's: refused with the bytes found, nothing written");
            f.seed(pc::kVerifyFrom);
            f.mem[20] = 0x31;   // the last byte of the call instruction
            check(run(f, p, true, true, seen) == pc::Event::kRefusedBytes && f.casCalls == 0, "F231: the whole 21 bytes are checked, the last one too");
            f.seed(pc::kVerifyFrom);
            f.interfere = 1;
            check(run(f, p, true, true, seen) == pc::Event::kRefusedWrite && !p.applied && std::memcmp(f.mem, pc::kVerifyFrom, pc::kVerifyLen) == 0, "F231: the qword changed under the write: the compare-exchange fails, nothing is written, not applied");
            check(run(f, p, true, true, seen) == pc::Event::kApplied && p.applied, "F231: ... and the next tick applies it");
            f.interfere = 1;
            check(run(f, p, false, true, seen) == pc::Event::kRestoreRetry && p.applied && std::memcmp(f.mem, pc::kVerifyTo, pc::kVerifyLen) == 0, "F231: a restore whose compare-exchange fails stays applied and is retried");
            check(run(f, p, false, true, seen) == pc::Event::kRestored && !p.applied, "F231: ... and succeeds next tick");
            f.seed(pc::kVerifyFrom);
            pc::Patch q;
            run(f, q, true, true, seen);
            f.mem[5] = 0x77;   // another writer changed the patched bytes
            const int calls = f.casCalls;
            check(run(f, q, false, true, seen) == pc::Event::kNotRestoredBytes && !q.applied && f.casCalls == calls, "F231: bytes that no longer read as ours are not written back: the patch is let go");
            f.seed(pc::kVerifyFrom);
            pc::Patch r;
            run(f, r, true, true, seen);
            f.readable = false;
            check(run(f, r, false, true, seen) == pc::Event::kNone && r.applied, "F231: an unreadable restore is retried next frame, the patch stays ours");
        }
        char line[1100];
        uint8_t shown[pc::kVerifyLen];
        std::memcpy(shown, pc::kVerifyFrom, sizeof(shown));
        shown[4] = 0x44;
        pc::formatEvent(line, sizeof(line), pc::Event::kApplied, "", shown);
        const std::string applied = line;
        pc::formatEvent(line, sizeof(line), pc::Event::kRestored, "the game session ended", shown);
        const std::string restored = line;
        pc::formatEvent(line, sizeof(line), pc::Event::kRefusedBytes, "", shown);
        const std::string refusedBytes = line;
        pc::formatEvent(line, sizeof(line), pc::Event::kNotRestoredBytes, "native stereo on foot ended", shown);
        const std::string notRestored = line;
        check(applied == "native on foot preview camera (F231, TEMPORARY): patch EliteDangerous64.exe+0x126FE70 45 33 C0 -> 41 B0 01 (every preview client's camera request is forced; written as one interlocked qword)" &&
                  restored == "native on foot preview camera (F231, TEMPORARY): patch EliteDangerous64.exe+0x126FE70 41 B0 01 -> 45 33 C0 (restored: the game session ended)" &&
                  has_(refusedBytes, "NOT patched -- the 21 bytes at +0x126FE6C are 48 8B 53 50 44 33 C0") && has_(refusedBytes, "not 48 8B 53 50 45 33 C0 4C 8B 08") && has_(refusedBytes, "someone else's and are left alone") &&
                  has_(notRestored, "NOT restored -- the bytes are 48 8B 53 50 44 33 C0") && has_(notRestored, "not ours (restore: native stereo on foot ended)"),
              "F231: the applied, restored, refused and not-restored lines name the address, the shape, the bytes found and expected, and why");
        pc::formatEvent(line, sizeof(line), pc::Event::kRefusedIdentity, "", shown);
        const std::string ident = line;
        pc::formatEvent(line, sizeof(line), pc::Event::kRestoreRetry, "the game session ended", shown);
        check(has_(ident, "NOT patched -- the game build differs") && has_(std::string(line), "could not be restored (the game session ended); retrying each frame."), "F231: the identity refusal and the restore retry say so");
        pc::formatKey(line, sizeof(line), true);
        const std::string keyOn = line;
        pc::formatKey(line, sizeof(line), false);
        check(keyOn.rfind("native on foot preview camera (F231, TEMPORARY): experimental.native_on_foot_preview_camera is on -- ", 0) == 0 && has_(keyOn, "show the preview from their own camera") &&
                  has_(std::string(line), "experimental.native_on_foot_preview_camera is off -- the game's camera request is left as it is"),
              "F231: the key's line says what each value does");
        pc::CallEvent ev;
        ev.kind = pc::kSetCamera;
        ev.tid = 7;
        ev.atMs = 123456;
        ev.retRva = 0x126FE81;
        ev.force = 1;
        std::snprintf(ev.name, sizeof(ev.name), "Livery Camera");
        pc::formatCall(line, sizeof(line), ev);
        const std::string setCam = line;
        ev.retRva = 0x1234;
        ev.viaObject = true;
        ev.force = 0;
        pc::formatCall(line, sizeof(line), ev);
        const std::string setOther = line;
        ev.kind = pc::kRelease;
        pc::formatCall(line, sizeof(line), ev);
        check(setCam == "preview camera (F231): SetCamera at 123456 ms, thread 7, called from +0x126FE81 (a preview client), camera name \"Livery Camera\", force 1" &&
                  has_(setOther, "called from +0x1234 (another caller)") && has_(setOther, "(read through a string object), force 0") &&
                  std::string(line) == "preview camera (F231): Release at 123456 ms, thread 7, called from +0x1234",
              "F231: the SetCamera line names the time, thread, return address (a preview client at +0x126FE81), the name and the force byte; the Release line the time, thread and return address");
        char name[pc::kNameCopy];
        std::memset(name, 'A', sizeof(name));
        std::memcpy(name, "ok\x01\"\xFF" "z", 6);
        name[sizeof(name) - 1] = 0;
        pc::sanitizeName(name, sizeof(name));
        char empty[4] = {0, 0, 0, 0};
        char text[4] = {'a', 0, 0, 0};
        check(std::strncmp(name, "ok???z", 6) == 0 && std::strlen(name) == sizeof(name) - 1 && !pc::looksLikeText(empty) && pc::looksLikeText(text) && pc::isPreviewCaller(0x126FE81) && !pc::isPreviewCaller(0x126FE80),
              "F231: a name copy shows control characters, quotes and high bytes as '?', stays bounded, and an empty or non-text copy is not text");
        pc::formatCapped(line, sizeof(line), "SetCamera", 12, 40);
        const std::string capped = line;
        pc::formatLost(line, sizeof(line), 3);
        check(capped == "preview camera (F231): 12 SetCamera lines in the last 10 s were not written (at most 40 in any ten seconds)" &&
                  std::string(line) == "preview camera (F231): 3 call(s) were not recorded (the ring was full or a second hook thread was in it)",
              "F231: the cap's note and the lost-calls note name what was dropped");
        // the wiring, read as text
        const std::string hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"), cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini");
        check(has_(hooks, "case kHkSetCamera: return reinterpret_cast<const void*>(&previewSetCameraHooked);") && has_(hooks, "case kHkReleaseCam: return reinterpret_cast<const void*>(&previewReleaseHooked);") &&
                  has_(hooks, "{\"preview-camera-set\"") && has_(hooks, "{\"preview-camera-release\"") && has_(hooks, "g_gate[kHkSetCamera].store(armed(kHkSetCamera) && capture ? 1u : 0u, std::memory_order_release);") &&
                  has_(hooks, "tryHook(kHkSetCamera, sink);") && has_(hooks, "tryHook(kHkReleaseCam, sink);") && has_(hooks, "reinterpret_cast<uintptr_t>(_ReturnAddress())") &&
                  countOf(hooks, "return forward(a, b, c, d);") >= 3,
              "F231: the two log-only hooks are in the hook table, gated and tried with the others, read the caller's return address and forward every call");
        check(has_(cpp, "cfg.getString(\"experimental.native_on_foot_preview_camera\", \"on\") != \"off\"") && has_(cpp, "previewcam::formatKey(line, sizeof(line), on);") && has_(cpp, "InterlockedCompareExchange64(") &&
                  has_(cpp, "previewcam::notWantedWhy(g_previewCamKey.load(std::memory_order_relaxed), nativeOnFootActive(), g_hudOverrideHeld, session);") &&
                  has_(cpp, "previewCameraPatchTick(false, \"the game is shutting down\");") && has_(cpp, "str(\"experimental.native_on_foot_preview_camera\""),
              "F231: the key is read live, the patch is written by one interlocked compare-exchange on the panel patch's condition, put back at shutdown, and the key is dumped");
        check(has_(ini, "# ui: Preview screens use their camera | choices on, off | recommended on | live\nnative_on_foot_preview_camera = on\n") && has_(ini, "# TEMPORARY (F231): Preview screens use their camera") &&
                  has_(ini, "the preview screens (Livery, the shops, the loadout) show the preview from their own camera") && ini.find("native_on_foot_preview_camera = on") > ini.find("native_on_foot_livery_camera_repair = on"),
              "F231: the shipped key is on, TEMPORARY, with its ui line after F230's and the plain sentence of what the user gets");
    }
    // ---- 11. F232: the preview handover (TEMPORARY key): the key's line and the wiring (the roster check itself is run by native_preview_camera_test) -----------------------------------------------
    {
        namespace pc = edvr::previewcam;
        char line[700];
        pc::formatHandoverKey(line, sizeof(line), true);
        const std::string on = line;
        pc::formatHandoverKey(line, sizeof(line), false);
        check(std::strcmp(pc::kHandoverKey, "experimental.native_on_foot_preview_handover") == 0 &&
                  on.rfind("native on foot preview handover (F232, TEMPORARY): experimental.native_on_foot_preview_handover is on -- ", 0) == 0 && has_(on, "two to six producers") &&
                  has_(std::string(line), "is off -- the preview camera accepts a Location camera list of exactly two producers, as before"),
              "F232: the key's line says what each value does");
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), cam = slurp("src/d3d11/native_preview_camera.cpp"), core = slurp("src/d3d11/native_preview_camera_core.h"),
                          hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"), ini = slurp("edvr.ini");
        check(has_(cpp, "cfg.getString(\"experimental.native_on_foot_preview_handover\", \"on\") != \"off\"") && has_(cpp, "previewcam::formatHandoverKey(line, sizeof(line), on);") &&
                  has_(cpp, "nativePreviewCameraSetHandover(on);") && has_(cpp, "nativePreviewCameraDiagTick(nativeOnFootPreviewScreenOpen());") && has_(cpp, "str(\"experimental.native_on_foot_preview_handover\""),
              "F232: the key is read live on the frame thread, default on, logged on a change, set into the preview camera, the diagnostic ticks with the watched screens' reading, and the key is dumped");
        check(has_(cam, "void nativePreviewCameraSetHandover(bool on) noexcept {g_previewGame.handover.store(on,std::memory_order_relaxed);}") &&
                  has_(cam, "g_previewEngine.diagWanted.store(wanted,std::memory_order_relaxed);") && has_(cam, "g_diagCap.allow(now)") && has_(cam, "if(g_diagAt&&now-g_diagAt<1000)return;"),
              "F232: the switch is one relaxed atomic store, the snapshot is wanted only while a watched screen is open, and the line is written once a second under the cap");
        check(has_(core, "inline bool identity(const Memory& m,const Game& g,uintptr_t child,Identity& out) noexcept {\n    Refuse why=Refuse::kNone;\n    return identityWhy(m,g,child,out,why);\n}") &&
                  has_(core, "const bool handover=g.handover.load(std::memory_order_relaxed);") && has_(core, "if(handover&&count!=2) {") && core.find("Log::get()") == std::string::npos &&
                  core.find("std::mutex") == std::string::npos && has_(core, "if(d.busy.test_and_set(std::memory_order_acquire))return;"),
              "F232: identity() is identityWhy(), a count of two takes the original path with the key on, the snapshot is a try-claim that is skipped when busy, and the core logs and locks nothing");
        check(has_(hooks, "bool nativeOnFootPreviewScreenOpen() noexcept {") && has_(hooks, "g_shopOpen[shop::kOutfitting].load(std::memory_order_acquire) != 0"),
              "F232: the watched preview screens' reading is the two buy screens, the two Livery screens and the loadout browser");
        check(has_(ini, "# ui: Preview handover | choices on, off | recommended on | live\nnative_on_foot_preview_handover = on\n") && has_(ini, "# TEMPORARY (F232): Preview handover") &&
                  has_(ini, "a preview screen opened from another (Livery from the loadout browser) keeps its preview camera") && ini.find("native_on_foot_preview_handover = on") > ini.find("native_on_foot_preview_camera = on"),
              "F232: the shipped key is on, TEMPORARY, with its ui line after F231's and the plain sentence of what the user gets");
    }
}

}  // namespace f209test
