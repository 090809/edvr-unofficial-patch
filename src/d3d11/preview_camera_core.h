// F231 (native on-foot VR, TEMPORARY key experimental.native_on_foot_preview_camera): preview screens (Livery, the shops, the loadout) show their preview from their own camera. The pure half,
// header only, no Windows and no game: native_frame_test runs these functions (f209_checks.h), native_on_foot.cpp applies the patch through them, native_on_foot_hooks.cpp logs the two calls.
//
// WHY (reverse engineering of build 334925, inbox 20261109-112804-livery-activation). A preview client activates its camera in 0x126FE50, which calls the local camera controller's slot +0x30,
// SetCamera (0x1D7BEC0), with the client's camera name and a force flag of 0 (`xor r8d,r8d` at 0x126FE70). In the headset's mode SetCamera tries only "HMD <name>"; when that camera does not exist
// and the force flag (r8's low byte, `movzx r15d,r8b` at 0x1D7BEDE, the only read of r8 before r8 is overwritten at 0x1D7BF6B: checked in the exe) is 0, it selects nothing and "HMD First Person
// Camera" stays, so the preview never takes the frame. Release (0x1D7C1E0) always selects the first-person camera.
//
// THE PATCH. The three bytes `45 33 C0` at 0x126FE70 become `41 B0 01` (mov r8b,1): every preview client's camera request is forced. Applied only while the key is on AND native stereo is on foot
// with the first-person HUD override held (the condition the panel patch follows); put back otherwise and at shutdown. The 21 bytes of the two instructions around it are verified before the write
// (the whole `mov rdx,[rbx+50h] | xor r8d,r8d | mov r9,[rax] | mov rcx,rax | mov rdx,[rdx+20h] | call [r9+30h]`), and the restore writes only while they still read as the patch left them. The three
// bytes lie inside the aligned qword at 0x126FE70 (the image base is 64 KB aligned), so the write is ONE interlocked compare-exchange of that qword: another thread executing those bytes sees the
// old instruction or the new one, never a mix; it compares against the qword as it was read, so a byte changed by someone else in the meantime makes the write fail (retried next frame).
//
// THE HOOKS (log only, no game calls, forward unchanged): SetCamera and Release, exact-build like F229/F230. The return-address RVA 0x126FE81 is a preview client's call.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace previewcam {

constexpr uintptr_t kPatchRva = 0x126FE70;                 // the xor r8d,r8d
constexpr uintptr_t kVerifyRva = 0x126FE6C;                // the first byte of the mov rdx,[rbx+50h] before it
constexpr size_t kVerifyLen = 21;
constexpr size_t kPatchLen = 3;
static_assert((kPatchRva & 7u) == 0, "the three patched bytes must start an aligned qword for the interlocked write");
constexpr uintptr_t kSetCameraRva = 0x1D7BEC0, kReleaseRva = 0x1D7C1E0, kPreviewCallerRva = 0x126FE81;
constexpr char kKey[] = "experimental.native_on_foot_preview_camera";

inline constexpr uint8_t kVerifyFrom[kVerifyLen] = {0x48, 0x8B, 0x53, 0x50, 0x45, 0x33, 0xC0, 0x4C, 0x8B, 0x08, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0x52, 0x20, 0x41, 0xFF, 0x51, 0x30};
inline constexpr uint8_t kVerifyTo[kVerifyLen] = {0x48, 0x8B, 0x53, 0x50, 0x41, 0xB0, 0x01, 0x4C, 0x8B, 0x08, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0x52, 0x20, 0x41, 0xFF, 0x51, 0x30};
inline constexpr uint8_t kFrom3[kPatchLen] = {0x45, 0x33, 0xC0}, kTo3[kPatchLen] = {0x41, 0xB0, 0x01};

// The hooks' prologues. CodeHook steals whole instructions covering five bytes: the first instruction of each is five bytes, so five are stolen; the other boundaries (6/7/9/11/13/16/20 and
// 6/10/13) carry no relative operand either.
constexpr size_t kSetCameraPrologueBytes = 20, kReleasePrologueBytes = 13;
inline constexpr uint8_t kSetCameraPrologue[kSetCameraPrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x50};
inline constexpr uint8_t kReleasePrologue[kReleasePrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9};

inline const char* tag() noexcept { return "native on foot preview camera (F231, TEMPORARY):"; }   // the key and the patch's lines
inline const char* hookTag() noexcept { return "preview camera (F231):"; }                          // the two hooks' lines

// ---- when the patch is wanted -------------------------------------------------------------------------------------------------------------------------------------------------
// nullptr when it is wanted; otherwise the reason (a literal) it is put back or not applied.
inline const char* notWantedWhy(bool keyOn, bool nativeOnFoot, bool hudOverrideHeld, bool session) noexcept {
    if (!keyOn) return "experimental.native_on_foot_preview_camera is off";
    if (!nativeOnFoot) return "native stereo on foot ended";
    if (!hudOverrideHeld) return "the first-person HUD override is off";
    if (!session) return "the game session ended";
    return nullptr;
}

// ---- the patch's step ---------------------------------------------------------------------------------------------------------------------------------------------------------
enum class Event : int {
    kNone = 0,
    kApplied,
    kRestored,
    kRefusedIdentity,
    kRefusedUnreadable,
    kRefusedBytes,
    kRefusedWrite,
    kNotRestoredBytes,     // the bytes are not ours: the patch is let go, nothing is written
    kRestoreRetry          // the restore's write failed: retried next frame
};
struct Patch {
    bool applied = false;  // frame thread only
};
inline bool bytesEqual(const uint8_t* a, const uint8_t* b, size_t n) noexcept { return std::memcmp(a, b, n) == 0; }
// The qword at kPatchRva with its first three bytes replaced (the other five untouched): `current` is the qword as read (little endian).
inline uint64_t withThreeBytes(uint64_t current, const uint8_t three[kPatchLen]) noexcept {
    uint8_t b[8];
    std::memcpy(b, &current, 8);
    std::memcpy(b, three, kPatchLen);
    uint64_t out;
    std::memcpy(&out, b, 8);
    return out;
}
// One tick. read(rva, out, n) -> bool reads the game's bytes (guarded); cas(rva, expected, desired) -> bool is the interlocked compare-exchange of the aligned qword at rva (true when it
// swapped). `seen` receives the verified bytes read (for the refusal line). The patch is applied only when every check holds and put back only while the bytes still read as ours.
template <class Read, class Cas>
inline Event step(Patch& p, bool want, bool identityOk, Read&& read, Cas&& cas, uint8_t seen[kVerifyLen]) {
    uint8_t cur[kVerifyLen] = {};
    uint8_t q[8] = {};
    if (want) {
        if (p.applied) return Event::kNone;
        if (!identityOk) return Event::kRefusedIdentity;
        if (!read(kVerifyRva, cur, kVerifyLen) || !read(kPatchRva, q, 8)) return Event::kRefusedUnreadable;
        std::memcpy(seen, cur, kVerifyLen);
        if (!bytesEqual(cur, kVerifyFrom, kVerifyLen)) return Event::kRefusedBytes;
        uint64_t expected;
        std::memcpy(&expected, q, 8);
        if (!cas(kPatchRva, expected, withThreeBytes(expected, kTo3))) return Event::kRefusedWrite;
        p.applied = true;
        return Event::kApplied;
    }
    if (!p.applied) return Event::kNone;
    if (!read(kVerifyRva, cur, kVerifyLen) || !read(kPatchRva, q, 8)) return Event::kNone;   // retried next frame
    std::memcpy(seen, cur, kVerifyLen);
    if (!bytesEqual(cur, kVerifyTo, kVerifyLen)) {
        p.applied = false;
        return Event::kNotRestoredBytes;
    }
    uint64_t expected;
    std::memcpy(&expected, q, 8);
    if (!cas(kPatchRva, expected, withThreeBytes(expected, kFrom3))) return Event::kRestoreRetry;
    p.applied = false;
    return Event::kRestored;
}

// ---- the lines ----------------------------------------------------------------------------------------------------------------------------------------------------------------
inline void hexText(char* out, size_t cap, const uint8_t* b, size_t n) noexcept {
    size_t at = 0;
    if (cap) out[0] = 0;
    for (size_t i = 0; i < n && at + 4 < cap; ++i) at += static_cast<size_t>(std::snprintf(out + at, cap - at, i ? " %02X" : "%02X", static_cast<unsigned>(b[i])));
}
// `why` is the reason for a restore or the cause of a refusal; `seen` the verified bytes (a refusal for the bytes only).
inline int formatEvent(char* buf, size_t cap, Event e, const char* why, const uint8_t seen[kVerifyLen]) noexcept {
    char found[3 * kVerifyLen + 4], want[3 * kVerifyLen + 4];
    hexText(found, sizeof(found), seen, kVerifyLen);
    hexText(want, sizeof(want), kVerifyFrom, kVerifyLen);
    switch (e) {
        case Event::kApplied:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX 45 33 C0 -> 41 B0 01 (every preview client's camera request is forced; written as one interlocked qword)", tag(),
                                 static_cast<unsigned long long>(kPatchRva));
        case Event::kRestored:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX 41 B0 01 -> 45 33 C0 (restored: %s)", tag(), static_cast<unsigned long long>(kPatchRva), why);
        case Event::kRefusedIdentity:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX NOT patched -- the game build differs (the PE identity check failed). The game's bytes are left as they are.", tag(),
                                 static_cast<unsigned long long>(kPatchRva));
        case Event::kRefusedUnreadable:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX NOT patched -- the bytes could not be read. The game's bytes are left as they are.", tag(), static_cast<unsigned long long>(kPatchRva));
        case Event::kRefusedBytes:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX NOT patched -- the %u bytes at +0x%llX are %s, not %s, so they are someone else's and are left alone. The game's bytes are left as they are.",
                                 tag(), static_cast<unsigned long long>(kPatchRva), static_cast<unsigned>(kVerifyLen), static_cast<unsigned long long>(kVerifyRva), found, want);
        case Event::kRefusedWrite:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX NOT patched -- the bytes are not writable or changed under the write (retried next frame). The game's bytes are left as they are.", tag(),
                                 static_cast<unsigned long long>(kPatchRva));
        case Event::kNotRestoredBytes:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX NOT restored -- the bytes are %s, not ours (restore: %s)", tag(), static_cast<unsigned long long>(kPatchRva), found, why);
        case Event::kRestoreRetry:
            return std::snprintf(buf, cap, "%s patch EliteDangerous64.exe+0x%llX could not be restored (%s); retrying each frame.", tag(), static_cast<unsigned long long>(kPatchRva), why);
        default:
            return std::snprintf(buf, cap, "%s no event", tag());
    }
}
inline int formatKey(char* buf, size_t cap, bool on) noexcept {
    return std::snprintf(buf, cap, "%s %s is %s -- %s", tag(), kKey, on ? "on" : "off",
                         on ? "while native stereo is on foot (the first-person HUD override held) every preview client's camera request is forced, so preview screens (Livery, shops, loadout) show the preview from their own camera"
                            : "the game's camera request is left as it is (a preview screen may show from the commander's own camera)");
}

// ---- the hooks' events --------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr size_t kNameCopy = 65;
enum Kind : uint8_t { kSetCamera = 1, kRelease = 2 };
struct CallEvent {
    uint8_t kind = 0;
    uint32_t tid = 0;
    uint64_t atMs = 0;
    uint64_t retRva = 0;       // the return address minus the image base (0 when it lies outside)
    uint8_t force = 0;         // SetCamera: r8's low byte
    bool viaObject = false;    // the name was read through a string object (the direct read was not text)
    char name[kNameCopy] = {};
};
inline bool isPreviewCaller(uint64_t retRva) noexcept { return retRva == kPreviewCallerRva; }
// A bounded name copy as text: anything outside printable ASCII becomes '?'.
inline void sanitizeName(char* name, size_t cap) noexcept {
    for (size_t i = 0; i < cap; ++i) {
        if (name[i] == 0) return;
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c > 0x7E || c == '"') name[i] = '?';
    }
    name[cap - 1] = 0;
}
inline bool looksLikeText(const char* name) noexcept {
    const unsigned char c = static_cast<unsigned char>(name[0]);
    return c >= 0x20 && c <= 0x7E;
}
inline int formatCall(char* buf, size_t cap, const CallEvent& e) noexcept {
    if (e.kind == kSetCamera)
        return std::snprintf(buf, cap, "%s SetCamera at %llu ms, thread %u, called from +0x%llX (%s), camera name \"%s\"%s, force %u", hookTag(), static_cast<unsigned long long>(e.atMs), e.tid,
                             static_cast<unsigned long long>(e.retRva), isPreviewCaller(e.retRva) ? "a preview client" : "another caller", e.name, e.viaObject ? " (read through a string object)" : "",
                             static_cast<unsigned>(e.force));
    return std::snprintf(buf, cap, "%s Release at %llu ms, thread %u, called from +0x%llX", hookTag(), static_cast<unsigned long long>(e.atMs), e.tid, static_cast<unsigned long long>(e.retRva));
}
inline int formatCapped(char* buf, size_t cap, const char* what, uint32_t dropped, uint32_t perWindow) noexcept {
    return std::snprintf(buf, cap, "%s %u %s lines in the last 10 s were not written (at most %u in any ten seconds)", hookTag(), dropped, what, perWindow);
}
inline int formatLost(char* buf, size_t cap, uint64_t lost) noexcept {
    return std::snprintf(buf, cap, "%s %llu call(s) were not recorded (the ring was full or a second hook thread was in it)", hookTag(), static_cast<unsigned long long>(lost));
}

}  // namespace previewcam
}  // namespace edvr
