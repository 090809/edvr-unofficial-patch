// The concourse camera test (TEMPORARY): see concourse_camera_test.h. This file is exact-build code (hook_signatures.py AUDIT_EXEMPT): it installs only when gameBuildKnown(),
// so its RVAs are build 334925's and go through no resolver. Its relay is a copy of kinematic_eval_hook.cpp's (that file stays untouched).
#include "concourse_camera_test.h"
#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include <windows.h>
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
    if(!entry.relay){Log::get().note("concourse camera test: no relay page could be reserved near 0x1EA9C0");return false;}
    buildRelay(entry.relay,entry.gate,entry.callback);
    if(!entry.hook.install(reinterpret_cast<void*>(base+entry.rva),entry.relay,nullptr,entry.name,&prepareRelay,&entry)) {
        VirtualFree(entry.relay,0,MEM_RELEASE);
        entry.relay=nullptr;
        return false;
    }
    entry.ready.store(true,std::memory_order_release);
    return true;   // process-lifetime storage: an in-flight call may be executing the relay
}

// ---- TEMPORARY concourse camera test (docs\concourse-cull-2026-11-05.md): moves view 0's camera for whole frames, to see whether cull, draws, constants and image follow.
// The hook is on 0x1EA9C0 (the frame view builder: this, RenderView*[] views, count), build 334925's address; offsets READ in decomp_1EA9C0_334925.txt, decomp_2811D80_334925.txt,
// decomp_281C310_334925.txt, decomp_281C4F0_334925.txt and the camera recompute 0x4F3870 (analysis\decomp\verify_20261106_concourse_probes.txt):
//   RenderView+0x18 u32 flags (view 0 = the only entry with flags & 3 and none of 0x1068000, 0x10000), RenderView+0x310 the camera object (world position floats at +0x30,
//   basis rows +0x00 +0x10 +0x20, READ as 0x4F3870's matrix products), this+0xF4B*8 the first view context, +0x1A940 its count, +0x40 + i*0x6A0 its view slots (+0x540 position,
//   +0x688 flags). The slot of an entry is the number of earlier entries that went to the first context (INFERRED from the partition loop in 0x1EA9C0).
// The camera is written before the forward and put back after it (modes 2 and 3), or at the next call (the held modes 4 and 5, whose move lasts the whole frame); 0x4F3870 is called
// after each write so the planes and the +0x60 copy agree (0x200EC0, which builds view 0's descriptor, does not recompute). Nothing here takes a lock or logs: the tick reads the
// snapshot. A fault in any read or write is caught.
constexpr uintptr_t kCtHookRva = 0x1EA9C0u, kCtRecomputeRva = 0x4F3870u;
constexpr uintptr_t kCtFlagsOff = 0x18, kCtCamOff = 0x310, kCtPosOff = 0x30, kCtCtxOff = 0xF4Bu * 8u, kCtCountOff = 0x1A940, kCtSlotsOff = 0x40, kCtSlotStride = 0x6A0,
                    kCtSlotPosOff = 0x540, kCtSlotFlagsOff = 0x688;
constexpr uint32_t kCtDropFlags = (1u << 24) | (1u << 18) | (1u << 17) | (1u << 15), kCtBit16 = 1u << 16;   // 0x1068000, 0x10000: RenderView+0x18 bits that leave the first context (READ in 0x1EA9C0)
constexpr float kCtLiftM = 30.0f;   // mode 2: along the world's y
alignas(8) std::atomic<uintptr_t> camTestGate{0};
std::atomic<uint32_t> g_ctMode{0};
std::atomic<bool> g_ctActing{false};
std::atomic<uint64_t> g_ctCalls{0}, g_ctActed{0}, g_ctNoView0{0}, g_ctMulti{0}, g_ctFaults{0}, g_ctHeldPutBack{0}, g_ctHeldRewritten{0};
std::atomic<uint32_t> g_ctRvCount{0}, g_ctMain{0}, g_ctFlags{0}, g_ctSlotIdx{0}, g_ctSlotFlags{0};
std::atomic<bool> g_ctSlotKnown{false}, g_ctWroteLast{false};
std::atomic<float> g_ctBefore[3], g_ctWritten[3], g_ctSlot[3];
__declspec(noinline) uintptr_t __fastcall camTestObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
HookEntry g_camTestEntry{"concourse-camera-test",kCtHookRva,reinterpret_cast<void*>(&camTestObserved),&camTestGate};

struct CtFrame {
    uintptr_t cam=0;
    int slot=-1;
    uint32_t mode=0;   // 2 lift or 3 turn when a write is wanted (the held modes 4 and 5 map to these)
    uint32_t flags=0;
    bool wrote=false;
    bool held=false;   // put back at the next call, not after the forward
    float row[3][3]{};
    float pos[3]{};
    float wroteRow[3][3]{};   // what the write left, to see at the next call whether the game has written the camera since
    float wrotePos[3]{};
};
// A held write, waiting for the next call. Only the hook touches it; the view builder runs once a frame (g_ctCalls against the frame count in the log says if that is wrong).
CtFrame g_ctHeld;
std::atomic<bool> g_ctHeldPending{false};

void ctRecompute(uintptr_t cam) noexcept {
    __try {
        using Fn=void (__fastcall*)(uintptr_t);
        const uintptr_t at=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+kCtRecomputeRva;   // known build only (the install refuses another)
        reinterpret_cast<Fn>(at)(cam);
    } __except(EXCEPTION_EXECUTE_HANDLER) {g_ctFaults.fetch_add(1,std::memory_order_relaxed);}
}

// Put back what was written: only the floats that were changed.
void ctRestore(const CtFrame& f) noexcept {
    if(!f.wrote||!f.cam)return;
    __try {
        float* pos=reinterpret_cast<float*>(f.cam+kCtPosOff);
        if(f.mode==2)for(int i=0;i<3;++i)pos[i]=f.pos[i];
        if(f.mode==3)for(int r=0;r<3;++r) {
            float* row=reinterpret_cast<float*>(f.cam+0x10u*r);
            for(int i=0;i<3;++i)row[i]=f.row[r][i];
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {g_ctFaults.fetch_add(1,std::memory_order_relaxed);}
    ctRecompute(f.cam);
}

// A held write goes back only if the camera still holds it. If the game writes the camera every frame, last frame's saved values would overwrite this frame's and the view would
// freeze for the window; the two counts say which engine this is, which the bake needs to know. Should the game have moved the camera relative to the +30 m, y still sits about
// 30 m up, and it comes off, so the move cannot build up frame on frame.
void ctPutBackHeld(const CtFrame& f) noexcept {
    if(!f.wrote||!f.cam)return;
    bool ours=true,tookOff=false;
    __try {
        float* pos=reinterpret_cast<float*>(f.cam+kCtPosOff);
        if(f.mode==2) {
            ours=pos[0]==f.wrotePos[0]&&pos[1]==f.wrotePos[1]&&pos[2]==f.wrotePos[2];
            if(!ours&&std::fabs(pos[1]-f.pos[1])>=kCtLiftM*0.5f) {
                pos[1]-=kCtLiftM;
                tookOff=true;
            }
        }
        if(f.mode==3)for(int r=0;r<3;++r) {
            const float* row=reinterpret_cast<const float*>(f.cam+0x10u*r);
            for(int i=0;i<3;++i)if(row[i]!=f.wroteRow[r][i])ours=false;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    (ours?g_ctHeldPutBack:g_ctHeldRewritten).fetch_add(1,std::memory_order_relaxed);
    if(ours)ctRestore(f);
    else if(tookOff)ctRecompute(f.cam);
}

void ctBefore(uintptr_t views,uintptr_t count,CtFrame& f) noexcept {
    f=CtFrame{};
    const uint32_t rawMode=g_ctMode.load(std::memory_order_relaxed);
    const uint32_t mode=rawMode>=4?rawMode-2:rawMode;
    f.mode=mode;
    f.held=rawMode>=4;
    if(!mode)return;
    g_ctCalls.fetch_add(1,std::memory_order_relaxed);
    g_ctWroteLast.store(false,std::memory_order_relaxed);
    __try {
        g_ctRvCount.store(static_cast<uint32_t>(count),std::memory_order_relaxed);
        if(!views||count==0||count>64) {g_ctNoView0.fetch_add(1,std::memory_order_relaxed);return;}
        const uintptr_t* array=reinterpret_cast<const uintptr_t*>(views);
        uint32_t groupA=0,mainCount=0;
        uintptr_t chosen=0;
        for(uintptr_t i=0;i<count;++i) {
            const uintptr_t rv=array[i];
            if(!rv)continue;
            const uint32_t flags=*reinterpret_cast<const uint32_t*>(rv+kCtFlagsOff);
            if((flags&kCtDropFlags)||(flags&kCtBit16))continue;   // not in the first context
            if(flags&3u) {
                if(++mainCount==1) {chosen=rv;f.slot=static_cast<int>(groupA);f.flags=flags;}
            }
            ++groupA;
        }
        g_ctMain.store(mainCount,std::memory_order_relaxed);
        if(mainCount!=1||!chosen) {
            f.slot=-1;
            (mainCount==0?g_ctNoView0:g_ctMulti).fetch_add(1,std::memory_order_relaxed);
            return;
        }
        const uintptr_t cam=chosen+kCtCamOff;
        const float* pos=reinterpret_cast<const float*>(cam+kCtPosOff);
        const float p0=pos[0],p1=pos[1],p2=pos[2];
        if(!(std::fabs(p0)<1.0e9f&&std::fabs(p1)<1.0e9f&&std::fabs(p2)<1.0e9f)) {   // also false for NaN
            g_ctNoView0.fetch_add(1,std::memory_order_relaxed);
            f.slot=-1;
            return;
        }
        f.cam=cam;
        g_ctFlags.store(f.flags,std::memory_order_relaxed);
        g_ctBefore[0].store(p0,std::memory_order_relaxed);g_ctBefore[1].store(p1,std::memory_order_relaxed);g_ctBefore[2].store(p2,std::memory_order_relaxed);
        g_ctWritten[0].store(p0,std::memory_order_relaxed);g_ctWritten[1].store(p1,std::memory_order_relaxed);g_ctWritten[2].store(p2,std::memory_order_relaxed);
        if(!g_ctActing.load(std::memory_order_relaxed)||(mode!=2&&mode!=3))return;
        f.pos[0]=p0;f.pos[1]=p1;f.pos[2]=p2;
        for(int r=0;r<3;++r) {
            const float* row=reinterpret_cast<const float*>(cam+0x10u*r);
            f.row[r][0]=row[0];f.row[r][1]=row[1];f.row[r][2]=row[2];
        }
        f.wrote=true;   // before the first write: a fault mid-write is put back
        if(mode==2) {
            reinterpret_cast<float*>(cam+kCtPosOff)[1]=p1+kCtLiftM;
            g_ctWritten[1].store(p1+kCtLiftM,std::memory_order_relaxed);
        } else {
            // 90 degrees about the world's y, each basis row (x,y,z) -> (z,y,-x). The rows are the world axes of the camera's local axes (READ in 0x4F3870's products).
            for(int r=0;r<3;++r) {
                float* row=reinterpret_cast<float*>(cam+0x10u*r);
                const float x=f.row[r][0],z=f.row[r][2];
                row[0]=z;row[2]=-x;
            }
        }
        for(int i=0;i<3;++i)f.wrotePos[i]=pos[i];
        for(int r=0;r<3;++r) {
            const float* row=reinterpret_cast<const float*>(cam+0x10u*r);
            for(int i=0;i<3;++i)f.wroteRow[r][i]=row[i];
        }
        g_ctActed.fetch_add(1,std::memory_order_relaxed);
        g_ctWroteLast.store(true,std::memory_order_relaxed);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ctFaults.fetch_add(1,std::memory_order_relaxed);
        f.slot=-1;
    }
    if(f.wrote)ctRecompute(f.cam);
}

void ctAfter(uintptr_t self,CtFrame& f) noexcept {
    if(!f.mode)return;
    if(f.slot>=0&&f.cam) {
        __try {
            const uintptr_t ctx=self+kCtCtxOff;
            const uint64_t n=*reinterpret_cast<const uint64_t*>(ctx+kCtCountOff);
            if(n>0&&n<=64&&static_cast<uint64_t>(f.slot)<n) {
                const uintptr_t slot=ctx+kCtSlotsOff+kCtSlotStride*static_cast<uintptr_t>(f.slot);
                const float* p=reinterpret_cast<const float*>(slot+kCtSlotPosOff);
                g_ctSlot[0].store(p[0],std::memory_order_relaxed);g_ctSlot[1].store(p[1],std::memory_order_relaxed);g_ctSlot[2].store(p[2],std::memory_order_relaxed);
                g_ctSlotFlags.store(*reinterpret_cast<const uint32_t*>(slot+kCtSlotFlagsOff),std::memory_order_relaxed);
                g_ctSlotIdx.store(static_cast<uint32_t>(f.slot),std::memory_order_relaxed);
                g_ctSlotKnown.store(true,std::memory_order_relaxed);
            } else g_ctSlotKnown.store(false,std::memory_order_relaxed);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            g_ctFaults.fetch_add(1,std::memory_order_relaxed);
            g_ctSlotKnown.store(false,std::memory_order_relaxed);
        }
    } else g_ctSlotKnown.store(false,std::memory_order_relaxed);
    if(f.held&&f.wrote) {
        g_ctHeld=f;   // the frame keeps the moved camera; the next call puts it back before it reads
        g_ctHeldPending.store(true,std::memory_order_release);
        return;
    }
    ctRestore(f);
}

__declspec(noinline) uintptr_t __fastcall camTestObserved(uintptr_t self,uintptr_t views,uintptr_t count) noexcept {
    const auto forward=reinterpret_cast<EvalFn>(g_camTestEntry.forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    if(g_ctHeldPending.load(std::memory_order_acquire)) {   // the last frame's held write goes back first, so this frame reads the game's own camera and nothing accumulates
        ctPutBackHeld(g_ctHeld);
        g_ctHeld=CtFrame{};
        g_ctHeldPending.store(false,std::memory_order_release);   // the tick's next control call closes the gate if the mode is 0
    }
    CtFrame f;
    ctBefore(views,count,f);
    const uintptr_t result=forward(self,views,count);
    ctAfter(self,f);
    return result;
}

// 0x1EA9C0's first 17 bytes (build 334925), READ in the disassembly: push rbp,rsi,rdi,r12..r15; mov eax,0x2B940.
bool ctPrologueOk(uintptr_t at) noexcept {
    static const uint8_t kExpect[17]={0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0xB8,0x40,0xB9,0x02,0x00};
    __try {return std::memcmp(reinterpret_cast<const void*>(at),kExpect,sizeof(kExpect))==0;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
} // namespace

// Installed once, only when hotkey.concourse_camera_test is bound (the caller decides), and only on build 334925: the offsets above were read there.
bool concourseCameraTestInstall(const char** why) noexcept {
    const char* sink=nullptr;
    if(!why)why=&sink;
    *why=nullptr;
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if(g_camTestEntry.ready.load(std::memory_order_acquire))return true;
        if(!gameBuildKnown()){*why="the game build is not 334925, the build these offsets were read on";return false;}
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base){*why="the module base could not be read";return false;}
        if(!ctPrologueOk(base+kCtHookRva)){*why="the prologue at 0x1EA9C0 is not the one that was read";return false;}
        if(!installOne(g_camTestEntry,base)){*why="CodeHook refused it (its own line names why)";return false;}
        return true;
    } catch(...) {*why="an exception during the install";return false;}
}

// The mode (0 off, 1 log only, 2 lift, 3 turn, 4 lift held, 5 turn held) and whether the camera may be written this frame. Called every tick. The relay's gate is open while the
// mode is not 0, or while a held write is still out: that must reach the next call, which puts it back. A write that comes out just as the mode goes to 0 reopens the gate a tick later.
void concourseCameraTestControl(uint32_t mode,bool acting) noexcept {
    g_ctMode.store(mode,std::memory_order_relaxed);
    g_ctActing.store(acting&&mode>=2,std::memory_order_relaxed);
    camTestGate.store((mode!=0||g_ctHeldPending.load(std::memory_order_acquire))?uintptr_t(1):uintptr_t(0),std::memory_order_release);
}

bool concourseCameraTestHooked() noexcept {return g_camTestEntry.ready.load(std::memory_order_acquire);}

void concourseCameraTestSnapshot(ConcourseCameraTestSnap* out) noexcept {
    if(!out)return;
    out->calls=g_ctCalls.load(std::memory_order_relaxed);
    out->acted=g_ctActed.load(std::memory_order_relaxed);
    out->noView0=g_ctNoView0.load(std::memory_order_relaxed);
    out->multiView=g_ctMulti.load(std::memory_order_relaxed);
    out->faults=g_ctFaults.load(std::memory_order_relaxed);
    out->heldPutBack=g_ctHeldPutBack.load(std::memory_order_relaxed);
    out->heldRewritten=g_ctHeldRewritten.load(std::memory_order_relaxed);
    out->rvCount=g_ctRvCount.load(std::memory_order_relaxed);
    out->mainCount=g_ctMain.load(std::memory_order_relaxed);
    out->flags=g_ctFlags.load(std::memory_order_relaxed);
    out->slotFlags=g_ctSlotFlags.load(std::memory_order_relaxed);
    out->slotIndex=g_ctSlotIdx.load(std::memory_order_relaxed);
    out->slotKnown=g_ctSlotKnown.load(std::memory_order_relaxed);
    out->wroteLast=g_ctWroteLast.load(std::memory_order_relaxed);
    for(int i=0;i<3;++i) {
        out->before[i]=g_ctBefore[i].load(std::memory_order_relaxed);
        out->written[i]=g_ctWritten[i].load(std::memory_order_relaxed);
        out->slot[i]=g_ctSlot[i].load(std::memory_order_relaxed);
    }
}


} // namespace edvr
