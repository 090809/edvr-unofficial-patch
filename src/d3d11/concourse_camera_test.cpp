// The concourse camera test (TEMPORARY): see concourse_camera_test.h. This file is exact-build code (hook_signatures.py AUDIT_EXEMPT): it installs only when gameBuildKnown(),
// so its RVAs are build 334925's and go through no resolver. Its relay is a copy of kinematic_eval_hook.cpp's (that file stays untouched).
#include "concourse_camera_test.h"
#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace edvr {
namespace {
using EvalFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t);
constexpr size_t kRelayBytes=44,kOriginalLiteral=36;
std::mutex g_installMutex;

struct HookEntry {
    const char* name;
    uintptr_t rva;
    void* callback;
    const void* gate=nullptr;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay=nullptr;
    std::atomic<bool> ready{false};
};

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    const uintptr_t granularity=info.dwAllocationGranularity;
    const uintptr_t floor=reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling=reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance=uintptr_t(INT32_MAX)-0x10000u;
    uintptr_t at=target>distance?target-distance:floor;
    if(at<floor)at=floor;
    const uintptr_t limit=target>ceiling-distance?ceiling:target+distance;
    while(at<limit) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)))break;
        const uintptr_t start=reinterpret_cast<uintptr_t>(region.BaseAddress);
        if(region.RegionSize>UINTPTR_MAX-start)break;
        const uintptr_t end=start+region.RegionSize;
        if(region.State==MEM_FREE) {
            uintptr_t candidate=at>start?at:start;
            if(candidate>UINTPTR_MAX-(granularity-1))break;
            candidate=(candidate+granularity-1)&~(granularity-1);
            if(candidate<limit && candidate<end && end-candidate>=4096) {
                auto* p=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                                                         MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
                if(p)return p;
            }
        }
        if(end<=at)break;at=end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code,const void* gate,void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback];
    // original: jmp [trampoline]. RAX/flags are volatile, and neither the
    // evaluator nor the job bodies consume RAX on entry. No stack adjustment
    // or nonvolatile modification occurs.
    const uint8_t body[kRelayBytes]={
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code,body,sizeof(body));
    const uintptr_t gateAddress=reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress=reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code+2,&gateAddress,8);std::memcpy(code+22,&callbackAddress,8);
}

bool prepareRelay(void* trampoline,void* context) noexcept {
    auto& entry=*static_cast<HookEntry*>(context);
    const uintptr_t address=reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay+kOriginalLiteral,&address,8);
    DWORD oldProtect=0;
    if(!VirtualProtect(entry.relay,4096,PAGE_EXECUTE_READ,&oldProtect) ||
       !FlushInstructionCache(GetCurrentProcess(),entry.relay,kRelayBytes))return false;
    entry.forward.store(address,std::memory_order_release);
    return true;
}
bool installOne(HookEntry& entry,uintptr_t base) noexcept {
    entry.relay=allocateRelay(base+entry.rva);
    if(!entry.relay){Log::get().note("concourse camera test: no relay page could be reserved near 0x%llX", static_cast<unsigned long long>(entry.rva));return false;}
    buildRelay(entry.relay,entry.gate,entry.callback);
    if(!entry.hook.install(reinterpret_cast<void*>(base+entry.rva),entry.relay,nullptr,entry.name,&prepareRelay,&entry)) {
        VirtualFree(entry.relay,0,MEM_RELEASE);
        entry.relay=nullptr;
        return false;
    }
    entry.ready.store(true,std::memory_order_release);
    return true;   // process-lifetime storage: an in-flight call may be executing the relay
}

// ---- TEMPORARY concourse camera test v2 (docs\concourse-cull-2026-11-05.md "Flight 20 results"): Flight 20 refuted a write to RenderView+0x310 after the engine built it (the game
// builds the eye cameras again every frame, and cb1 comes from another copy). This version moves the pose that goes INTO the camera builder, and names who gives view 0 its camera.
// Hooks (build 334925 RVAs; prologues pinned below from the exe; each installs alone, a refusal is logged):
//   0x1EA9C0 (this, RenderView*[], count): READ the view builder. Identifies view 0 (RenderView+0x18 flags & 3, none of 0x1068000 / 0x10000: READ) and reads its slot after the fill
//            (this+0xF4B*8 context, +0x1A940 count, +0x40 + i*0x6A0, +0x540 position: READ). The slot index is INFERRED from the partition loop.
//   0x2874760 (RenderView, size, camera): READ the snapshot that writes RV+0x310 (0x59F2A0(RV+0x310, camera)). The hook records _ReturnAddress() (the relay jumps, so it is the game's call site)
//            per RenderView in a lock-free table and in a histogram: that names the function that hands view 0 its camera, whatever path flat takes.
//   0x2845730 (renderer, pass, ctx): READ the builder that, in the VR path read, takes W+0x3330 (W = *(renderer+0x50)) as the pose, then resets it to the identity when pass != 1.
//            Counted; the pass value and the mailbox (translation, identity or not) are read before the forward. Whether it runs in flat: that is what the counts say.
//   0x283FA20 (renderer, pose4x4*, near, far, five more): READ the eye camera builder: eyeWorld = eyeOffset x pose (row-vector, rows = axes, row 3 = position). THE WRITE POINT: the main
//            composition passes a 4x4 on the caller's stack (0x2845730's local copy of the mailbox, lea rdx,[rbp-0x60] at 0x284587F: READ); the second passes the persistent W+0x130
//            (0x28458EA: READ). A pose pointer inside the 16 KiB above the stub's frame is a local copy (written, never put back: the caller does not read it again, INFERRED);
//            anything else is refused and counted. Near and far arrive in xmm2 and xmm3 (READ at both call sites), so this hook is a register-preserving stub, not a C++ forward.
// Nothing in a hook takes a lock or logs: the tick reads the snapshot. A fault in a read or write is caught.
constexpr uintptr_t kHkViews = 0x1EA9C0u, kHkSnap = 0x2874760u, kHkFlow = 0x2845730u, kHkEye = 0x283FA20u;
constexpr uintptr_t kCtFlagsOff = 0x18, kCtCamOff = 0x310, kCtPosOff = 0x30, kCtCtxOff = 0xF4Bu * 8u, kCtCountOff = 0x1A940, kCtSlotsOff = 0x40, kCtSlotStride = 0x6A0,
                    kCtSlotPosOff = 0x540, kCtSlotFlagsOff = 0x688, kCtRendererW = 0x50, kCtMailbox = 0x3330, kCtLocalWindow = 0x4000;
constexpr uint32_t kCtDropFlags = (1u << 24) | (1u << 18) | (1u << 17) | (1u << 15), kCtBit16 = 1u << 16;   // 0x1068000, 0x10000: RenderView+0x18 bits that leave the first context (READ in 0x1EA9C0)
constexpr float kCtLiftM = 30.0f;   // mode 2: along the world y
enum : int { kIdxViews, kIdxSnap, kIdxFlow, kIdxEye, kIdxCount };
alignas(8) std::atomic<uintptr_t> camTestGate{0};
std::atomic<uint32_t> g_ctMode{0};
std::atomic<bool> g_ctActing{false};
std::atomic<uint64_t> g_ctCalls[kIdxCount];
std::atomic<uint64_t> g_ctFaults{0}, g_ctNoView0{0}, g_ctMulti{0}, g_ctEyeWritten{0}, g_ctEyeRefused{0};
std::atomic<uint32_t> g_ctRvCount{0}, g_ctMain{0}, g_ctFlags{0}, g_ctSlotIdx{0}, g_ctSlotFlags{0}, g_ctPassLast{0xFFFFFFFFu}, g_ctPassMask{0}, g_ctMainRet{0}, g_ctRefusedHow{0};
std::atomic<bool> g_ctSlotKnown{false}, g_ctMailKnown{false}, g_ctMailIdentity{false};
std::atomic<float> g_ctRvPos[3], g_ctSlot[3], g_ctMailT[3], g_ctEyeBefore[16], g_ctEyeAfter[16];
constexpr int kRetSlots = 256, kHistSlots = 8, kEyeSiteSlots = 4;
struct RetSlot {std::atomic<uintptr_t> rv{0};std::atomic<uint32_t> ret{0};};
struct HistSlot {std::atomic<uint32_t> ret{0};std::atomic<uint64_t> n{0};};
RetSlot g_ctRets[kRetSlots];
HistSlot g_ctHist[kHistSlots];
HistSlot g_ctEyeSites[kEyeSiteSlots];
uintptr_t g_ctBase=0;

__declspec(noinline) uintptr_t __fastcall viewsObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall snapObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall flowObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
HookEntry g_ctEntries[kIdxCount]={
    {"concourse-camera-test-views",kHkViews,reinterpret_cast<void*>(&viewsObserved),&camTestGate},
    {"concourse-camera-test-snapshot",kHkSnap,reinterpret_cast<void*>(&snapObserved),&camTestGate},
    {"concourse-camera-test-flow",kHkFlow,reinterpret_cast<void*>(&flowObserved),&camTestGate},
    {"concourse-camera-test-eye",kHkEye,nullptr,nullptr},   // its own stub, ctEyeStub
};
// The prologues, READ from the exe (Ghidra, build 334925). A mismatch refuses that hook (an E9 first byte means another EDVR module already patched it).
struct Pin {int n;uint8_t b[24];};
const Pin kPins[kIdxCount]={
    {17,{0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0xB8,0x40,0xB9,0x02,0x00}},
    {18,{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x81,0xEC,0x70,0x02,0x00,0x00}},
    {17,{0x40,0x55,0x53,0x56,0x48,0x8D,0xAC,0x24,0x50,0xE8,0xFF,0xFF,0xB8,0xB0,0x18,0x00,0x00}},
    {21,{0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0xA8,0xFE,0xFF,0xFF}},
};

bool ctFiniteSmall(float v) noexcept {return std::fabs(v)<1.0e9f;}   // false for NaN too

void ctHistAdd(HistSlot* h,int n,uint32_t ret) noexcept {
    for(int i=0;i<n;++i) {
        uint32_t cur=h[i].ret.load(std::memory_order_relaxed);
        if(cur==0 && h[i].ret.compare_exchange_strong(cur,ret,std::memory_order_relaxed))cur=ret;
        if(cur==ret){h[i].n.fetch_add(1,std::memory_order_relaxed);break;}
    }
}
// The lock-free "who handed this RenderView its camera" table, and the histogram of distinct return RVAs.
void ctNoteRet(uintptr_t rv,uint32_t ret) noexcept {
    const uint32_t h=static_cast<uint32_t>((rv>>4)*2654435761u);
    for(uint32_t k=0;k<8;++k) {
        RetSlot& s=g_ctRets[(h+k)&(kRetSlots-1)];
        uintptr_t cur=s.rv.load(std::memory_order_relaxed);
        if(cur==0 && s.rv.compare_exchange_strong(cur,rv,std::memory_order_relaxed))cur=rv;
        if(cur==rv){s.ret.store(ret,std::memory_order_relaxed);break;}
    }
    ctHistAdd(g_ctHist,kHistSlots,ret);
}
uint32_t ctLookupRet(uintptr_t rv) noexcept {
    const uint32_t h=static_cast<uint32_t>((rv>>4)*2654435761u);
    for(uint32_t k=0;k<8;++k) {
        const RetSlot& s=g_ctRets[(h+k)&(kRetSlots-1)];
        const uintptr_t cur=s.rv.load(std::memory_order_relaxed);
        if(cur==rv)return s.ret.load(std::memory_order_relaxed);
        if(cur==0)break;
    }
    return 0;
}

// 0x1EA9C0 after the forward: which entry is view 0 and what its slot holds. Read only.
void ctReadViews(uintptr_t self,uintptr_t views,uintptr_t count) noexcept {
    __try {
        g_ctRvCount.store(static_cast<uint32_t>(count),std::memory_order_relaxed);
        if(!views||count==0||count>64) {g_ctNoView0.fetch_add(1,std::memory_order_relaxed);return;}
        const uintptr_t* array=reinterpret_cast<const uintptr_t*>(views);
        uint32_t groupA=0,mainCount=0;
        uintptr_t chosen=0;
        int slotIdx=-1;
        uint32_t chosenFlags=0;
        for(uintptr_t i=0;i<count;++i) {
            const uintptr_t rv=array[i];
            if(!rv)continue;
            const uint32_t flags=*reinterpret_cast<const uint32_t*>(rv+kCtFlagsOff);
            if((flags&kCtDropFlags)||(flags&kCtBit16))continue;   // not in the first context
            if(flags&3u) {
                if(++mainCount==1) {chosen=rv;slotIdx=static_cast<int>(groupA);chosenFlags=flags;}
            }
            ++groupA;
        }
        g_ctMain.store(mainCount,std::memory_order_relaxed);
        if(mainCount!=1||!chosen) {(mainCount==0?g_ctNoView0:g_ctMulti).fetch_add(1,std::memory_order_relaxed);return;}
        g_ctFlags.store(chosenFlags,std::memory_order_relaxed);
        const float* pos=reinterpret_cast<const float*>(chosen+kCtCamOff+kCtPosOff);
        for(int i=0;i<3;++i)g_ctRvPos[i].store(pos[i],std::memory_order_relaxed);
        g_ctMainRet.store(ctLookupRet(chosen),std::memory_order_relaxed);
        const uintptr_t ctx=self+kCtCtxOff;
        const uint64_t n=*reinterpret_cast<const uint64_t*>(ctx+kCtCountOff);
        if(n>0&&n<=64&&static_cast<uint64_t>(slotIdx)<n) {
            const uintptr_t slot=ctx+kCtSlotsOff+kCtSlotStride*static_cast<uintptr_t>(slotIdx);
            const float* p=reinterpret_cast<const float*>(slot+kCtSlotPosOff);
            for(int i=0;i<3;++i)g_ctSlot[i].store(p[i],std::memory_order_relaxed);
            g_ctSlotFlags.store(*reinterpret_cast<const uint32_t*>(slot+kCtSlotFlagsOff),std::memory_order_relaxed);
            g_ctSlotIdx.store(static_cast<uint32_t>(slotIdx),std::memory_order_relaxed);
            g_ctSlotKnown.store(true,std::memory_order_relaxed);
        } else g_ctSlotKnown.store(false,std::memory_order_relaxed);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
        g_ctSlotKnown.store(false,std::memory_order_relaxed);
    }
}

// 0x2845730 before the forward: the pass value and the mailbox. Read only.
void ctReadFlow(uintptr_t renderer,uintptr_t pass) noexcept {
    g_ctPassLast.store(static_cast<uint32_t>(pass),std::memory_order_relaxed);
    if(static_cast<uint32_t>(pass)<32)g_ctPassMask.fetch_or(1u<<static_cast<uint32_t>(pass),std::memory_order_relaxed);
    __try {
        const uintptr_t w=*reinterpret_cast<const uintptr_t*>(renderer+kCtRendererW);
        if(!w){g_ctMailKnown.store(false,std::memory_order_relaxed);return;}
        const float* m=reinterpret_cast<const float*>(w+kCtMailbox);
        bool identity=true;   // the reset value: rows 0-2 the identity's, row 3 all zero, its last float included (transition_flash_eye_base_core.h kResetMailbox)
        for(int i=0;i<16;++i) {
            const float want=(i==0||i==5||i==10)?1.0f:0.0f;
            if(!(std::fabs(m[i]-want)<1.0e-6f))identity=false;
        }
        for(int i=0;i<3;++i)g_ctMailT[i].store(m[12+i],std::memory_order_relaxed);
        g_ctMailIdentity.store(identity,std::memory_order_relaxed);
        g_ctMailKnown.store(true,std::memory_order_relaxed);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
        g_ctMailKnown.store(false,std::memory_order_relaxed);
    }
}

// 0x283FA20 before its body: the read, and in the window the write. `pose` is the 4x4 the caller passes; `frame` an address in the stub's frame. The pose is read in log only too:
// the RV camera against it says, with no write at all, whether this pose is the main camera in flat.
void ctEyeWrite(uintptr_t pose,uintptr_t frame,uint32_t mode) noexcept {
    const bool local=pose>frame&&pose-frame<kCtLocalWindow;
    if(!local) {   // the persistent second composition (W+0x130), or something unknown: not written
        g_ctEyeRefused.fetch_add(1,std::memory_order_relaxed);
        g_ctRefusedHow.store(pose==0?1u:2u,std::memory_order_relaxed);
        return;
    }
    __try {
        float* m=reinterpret_cast<float*>(pose);
        for(int i=0;i<16;++i)if(!ctFiniteSmall(m[i])){g_ctEyeRefused.fetch_add(1,std::memory_order_relaxed);g_ctRefusedHow.store(3u,std::memory_order_relaxed);return;}
        for(int i=0;i<16;++i)g_ctEyeBefore[i].store(m[i],std::memory_order_relaxed);
        if(!g_ctActing.load(std::memory_order_relaxed)||(mode!=2&&mode!=3)) {
            for(int i=0;i<16;++i)g_ctEyeAfter[i].store(m[i],std::memory_order_relaxed);
            return;
        }
        if(mode==2) {
            m[13]+=kCtLiftM;
        } else {
            // 90 degrees about the world y: each axis row (x,y,z) -> (z,y,-x). The rows are the camera axes in the world (READ in 0x283FA20's products).
            for(int r=0;r<3;++r) {
                float* row=m+4*r;
                const float x=row[0],z=row[2];
                row[0]=z;row[2]=-x;
            }
        }
        for(int i=0;i<16;++i)g_ctEyeAfter[i].store(m[i],std::memory_order_relaxed);
        g_ctEyeWritten.fetch_add(1,std::memory_order_relaxed);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
    }
}

template<int I> uintptr_t ctForward3(uintptr_t a,uintptr_t b,uintptr_t c) noexcept {
    const auto forward=reinterpret_cast<EvalFn>(g_ctEntries[I].forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    return forward(a,b,c);
}

__declspec(noinline) uintptr_t __fastcall viewsObserved(uintptr_t self,uintptr_t views,uintptr_t count) noexcept {
    g_ctCalls[kIdxViews].fetch_add(1,std::memory_order_relaxed);
    const uintptr_t result=ctForward3<kIdxViews>(self,views,count);
    ctReadViews(self,views,count);
    return result;
}
__declspec(noinline) uintptr_t __fastcall snapObserved(uintptr_t rv,uintptr_t size,uintptr_t camera) noexcept {
    g_ctCalls[kIdxSnap].fetch_add(1,std::memory_order_relaxed);
    const uintptr_t ret=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(g_ctBase&&ret>g_ctBase)ctNoteRet(rv,static_cast<uint32_t>(ret-g_ctBase));
    return ctForward3<kIdxSnap>(rv,size,camera);
}
__declspec(noinline) uintptr_t __fastcall flowObserved(uintptr_t renderer,uintptr_t pass,uintptr_t ctx) noexcept {
    g_ctCalls[kIdxFlow].fetch_add(1,std::memory_order_relaxed);
    ctReadFlow(renderer,pass);
    return ctForward3<kIdxFlow>(renderer,pass,ctx);
}
// Called by the stub with the caller's rdx. On entry [frame] is the stub's return, then its 0x88 of locals, its eight pushes, then the game's return.
__declspec(noinline) void ctEyeApply(uintptr_t pose) noexcept {
    const uint32_t mode=g_ctMode.load(std::memory_order_relaxed);
    if(mode==0)return;
    g_ctCalls[kIdxEye].fetch_add(1,std::memory_order_relaxed);
    const uintptr_t frame=reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
    const uintptr_t site=*reinterpret_cast<const uintptr_t*>(frame+0xD0);
    if(g_ctBase&&site>g_ctBase)ctHistAdd(g_ctEyeSites,kEyeSiteSlots,static_cast<uint32_t>(site-g_ctBase));
    ctEyeWrite(pose,frame,mode);
}
// A copy of native_preview_camera.cpp's eye stub, flown in VR. Entered by a jump from the patched entry, so the game's return stays on the stack and RSP is 8 mod 16; eight pushes keep
// that, sub rsp,0x88 leaves 0 mod 16 for the call, and xmm0-5 are saved. Everything is restored and the original body is jumped to: only the memory rdx points at may change.
constexpr unsigned kEyeApplyImm=59,kEyeTrampImm=120,kEyeStubBytes=130;
void* g_ctEyeTrampSlot=nullptr;
uint8_t* ctEyeStub(uintptr_t target) noexcept {
    uint8_t* page=allocateRelay(target);
    if(!page)return nullptr;
    const uint8_t code[]={
        0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x9C,
        0x48,0x81,0xEC,0x88,0x00,0x00,0x00,
        0x0F,0x29,0x44,0x24,0x20,0x0F,0x29,0x4C,0x24,0x30,0x0F,0x29,0x54,0x24,0x40,
        0x0F,0x29,0x5C,0x24,0x50,0x0F,0x29,0x64,0x24,0x60,0x0F,0x29,0x6C,0x24,0x70,
        0x48,0x8B,0x8C,0x24,0xB0,0x00,0x00,0x00,
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0xFF,0xD0,
        0x0F,0x28,0x44,0x24,0x20,0x0F,0x28,0x4C,0x24,0x30,0x0F,0x28,0x54,0x24,0x40,
        0x0F,0x28,0x5C,0x24,0x50,0x0F,0x28,0x64,0x24,0x60,0x0F,0x28,0x6C,0x24,0x70,
        0x48,0x81,0xC4,0x88,0x00,0x00,0x00,
        0x9D,0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5A,0x59,0x58,
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0xFF,0xE0};
    static_assert(sizeof(code)==kEyeStubBytes,"eye stub size");
    std::memcpy(page,code,sizeof(code));
    const auto apply=reinterpret_cast<uintptr_t>(&ctEyeApply);
    std::memcpy(page+kEyeApplyImm,&apply,sizeof(apply));
    g_ctEyeTrampSlot=page+kEyeTrampImm;
    return page;
}
bool ctEyePrepare(void* trampoline,void*) noexcept {
    if(!g_ctEyeTrampSlot||!trampoline)return false;
    std::memcpy(g_ctEyeTrampSlot,&trampoline,sizeof(trampoline));
    DWORD oldProtect=0;
    auto* page=static_cast<uint8_t*>(g_ctEyeTrampSlot)-kEyeTrampImm;
    return VirtualProtect(page,4096,PAGE_EXECUTE_READ,&oldProtect)&&FlushInstructionCache(GetCurrentProcess(),page,kEyeStubBytes);
}
bool ctInstallEye(HookEntry& entry,uintptr_t base) noexcept {
    entry.relay=ctEyeStub(base+entry.rva);
    if(!entry.relay){Log::get().note("concourse camera test: no stub page could be reserved near 0x%llX", static_cast<unsigned long long>(entry.rva));return false;}
    if(!entry.hook.install(reinterpret_cast<void*>(base+entry.rva),entry.relay,nullptr,entry.name,&ctEyePrepare,nullptr)) {
        VirtualFree(entry.relay,0,MEM_RELEASE);
        entry.relay=nullptr;
        return false;
    }
    entry.ready.store(true,std::memory_order_release);
    return true;
}

bool ctPinOk(int i,uintptr_t at) noexcept {
    __try {return std::memcmp(reinterpret_cast<const void*>(at),kPins[i].b,static_cast<size_t>(kPins[i].n))==0;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ctAlreadyPatched(uintptr_t at) noexcept {
    __try {return *reinterpret_cast<const uint8_t*>(at)==0xE9;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
std::atomic<uint32_t> g_ctHooked{0};
} // namespace

// Installed once, only when hotkey.concourse_camera_test is bound (the caller decides), and only on build 334925. Each hook installs alone; true when at least the eye hook or the
// view builder is in. A refusal is one log line naming the hook.
bool concourseCameraTestInstall(const char** why) noexcept {
    const char* sink=nullptr;
    if(!why)why=&sink;
    *why=nullptr;
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if(g_ctHooked.load(std::memory_order_acquire)!=0)return true;
        if(!gameBuildKnown()){*why="the game build is not 334925, the build these offsets were read on";return false;}
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base){*why="the module base could not be read";return false;}
        g_ctBase=base;
        uint32_t mask=0;
        for(int i=0;i<kIdxCount;++i) {
            const uintptr_t at=base+g_ctEntries[i].rva;
            if(ctAlreadyPatched(at)) {
                Log::get().note("concourse camera test: %s: 0x%llX is already patched by another EDVR module (the VR path?); skipped", g_ctEntries[i].name,static_cast<unsigned long long>(g_ctEntries[i].rva));
                continue;
            }
            if(!ctPinOk(i,at)) {
                Log::get().note("concourse camera test: %s: the prologue at 0x%llX is not the one that was read; skipped", g_ctEntries[i].name,static_cast<unsigned long long>(g_ctEntries[i].rva));
                continue;
            }
            if(!(i==kIdxEye?ctInstallEye(g_ctEntries[i],base):installOne(g_ctEntries[i],base))) {
                Log::get().note("concourse camera test: %s: CodeHook refused 0x%llX (its own line names why); skipped", g_ctEntries[i].name,static_cast<unsigned long long>(g_ctEntries[i].rva));
                continue;
            }
            mask|=1u<<i;
        }
        g_ctHooked.store(mask,std::memory_order_release);
        if(mask==0){*why="no hook could be installed";return false;}
        return true;
    } catch(...) {*why="an exception during the install";return false;}
}

// The mode (0 off, 1 log only, 2 lift, 3 turn) and whether the pose may be written now. The relay gate is open while the mode is not 0.
void concourseCameraTestControl(uint32_t mode,bool acting) noexcept {
    g_ctMode.store(mode,std::memory_order_relaxed);
    g_ctActing.store(acting&&(mode==2||mode==3),std::memory_order_relaxed);
    camTestGate.store(mode!=0?uintptr_t(1):uintptr_t(0),std::memory_order_release);
}

bool concourseCameraTestHooked() noexcept {return g_ctHooked.load(std::memory_order_acquire)!=0;}

void concourseCameraTestSnapshot(ConcourseCameraTestSnap* out) noexcept {
    if(!out)return;
    out->hookMask=g_ctHooked.load(std::memory_order_acquire);
    for(int i=0;i<kIdxCount;++i)out->calls[i]=g_ctCalls[i].load(std::memory_order_relaxed);
    out->eyeWritten=g_ctEyeWritten.load(std::memory_order_relaxed);
    out->eyeRefused=g_ctEyeRefused.load(std::memory_order_relaxed);
    out->refusedHow=g_ctRefusedHow.load(std::memory_order_relaxed);
    out->noView0=g_ctNoView0.load(std::memory_order_relaxed);
    out->multiView=g_ctMulti.load(std::memory_order_relaxed);
    out->faults=g_ctFaults.load(std::memory_order_relaxed);
    out->rvCount=g_ctRvCount.load(std::memory_order_relaxed);
    out->mainCount=g_ctMain.load(std::memory_order_relaxed);
    out->flags=g_ctFlags.load(std::memory_order_relaxed);
    out->slotFlags=g_ctSlotFlags.load(std::memory_order_relaxed);
    out->slotIndex=g_ctSlotIdx.load(std::memory_order_relaxed);
    out->slotKnown=g_ctSlotKnown.load(std::memory_order_relaxed);
    out->passLast=g_ctPassLast.load(std::memory_order_relaxed);
    out->passMask=g_ctPassMask.load(std::memory_order_relaxed);
    out->mailKnown=g_ctMailKnown.load(std::memory_order_relaxed);
    out->mailIdentity=g_ctMailIdentity.load(std::memory_order_relaxed);
    out->mainRet=g_ctMainRet.load(std::memory_order_relaxed);
    for(int i=0;i<3;++i) {
        out->rvPos[i]=g_ctRvPos[i].load(std::memory_order_relaxed);
        out->slot[i]=g_ctSlot[i].load(std::memory_order_relaxed);
        out->mailT[i]=g_ctMailT[i].load(std::memory_order_relaxed);
    }
    for(int i=0;i<16;++i) {
        out->eyeBefore[i]=g_ctEyeBefore[i].load(std::memory_order_relaxed);
        out->eyeAfter[i]=g_ctEyeAfter[i].load(std::memory_order_relaxed);
    }
    for(int i=0;i<ConcourseCameraTestSnap::kHist;++i) {
        out->histRet[i]=g_ctHist[i].ret.load(std::memory_order_relaxed);
        out->histN[i]=g_ctHist[i].n.load(std::memory_order_relaxed);
    }
    for(int i=0;i<ConcourseCameraTestSnap::kEyeSites;++i) {
        out->eyeSiteRet[i]=g_ctEyeSites[i].ret.load(std::memory_order_relaxed);
        out->eyeSiteN[i]=g_ctEyeSites[i].n.load(std::memory_order_relaxed);
    }
}

} // namespace edvr
