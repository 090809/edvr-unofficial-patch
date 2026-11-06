#pragma once
// The concourse's glass panes (docs\concourse-cull-2026-11-05.md, "The panes"). TEMPORARY instrument and test: while the commander is on foot in a
// station's social space, a draw named by its vertex shader, pixel shader and index count is drawn with another blend state around the game's own
// issue (tint, mirror or grey), and one frame's blended draws can be ranked by the pixels they cover (hotkey.concourse_pane_capture). Tint and grey also
// bind a white pixel shader, because a BLEND_FACTOR source multiplies whatever the shader writes: with the game's shader a dark pane stays dark whatever the
// factor. Mirror keeps the game's shader. The game's blend state and pixel shader are put back straight after the draw.
//
// The draw thunks of vscreen.cpp (hookedDrawIndexed and hookedDrawIndexedInstanced, the flat branch and the VR path) build one ConcoursePaneScope
// around the real draw call only. With nothing armed and the list empty the scope is one relaxed atomic load.
#include <d3d11.h>

#include <atomic>
#include <cstdint>

namespace edvr {

namespace detail {
extern std::atomic<bool> g_concoursePaneArmed;   // a capture frame is open, or a mode other than off has a list to treat (written by the frame tick)
}

// The game's own OMSetBlendState entry point, past EDVR's thunk (so the binding shadow and the issue epoch do not move; the state is put back
// before anything else looks).
using ConcoursePaneSetBlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
// The game's own PSSetShader entry point, past EDVR's thunk, for the same reason.
using ConcoursePaneSetPsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);

class ConcoursePaneScope {
public:
    ConcoursePaneScope(ID3D11DeviceContext* ctx, UINT indexCount, ConcoursePaneSetBlendFn setBlend, ConcoursePaneSetPsFn setPs) noexcept {
        if (detail::g_concoursePaneArmed.load(std::memory_order_relaxed)) begin(ctx, indexCount, setBlend, setPs);
    }
    ~ConcoursePaneScope() {
        if (state_ != 0) end();
    }
    ConcoursePaneScope(const ConcoursePaneScope&) = delete;
    ConcoursePaneScope& operator=(const ConcoursePaneScope&) = delete;

private:
    void begin(ID3D11DeviceContext* ctx, UINT indexCount, ConcoursePaneSetBlendFn setBlend, ConcoursePaneSetPsFn setPs) noexcept;
    void end() noexcept;

    ID3D11DeviceContext* ctx_ = nullptr;
    uint32_t state_ = 0;   // 0 nothing, 1 a capture query is open around the draw, 2 the blend state is replaced
    int query_ = -1;
    ID3D11BlendState* prev_ = nullptr;
    FLOAT prevFactor_[4] = {};
    UINT prevMask_ = 0;
    ConcoursePaneSetBlendFn setBlend_ = nullptr;
    ID3D11PixelShader* prevPs_ = nullptr;
    ConcoursePaneSetPsFn setPs_ = nullptr;   // set only while the white pixel shader is bound
};

// The frame tick, once a frame at the Present boundary (device_hook.cpp presentFrameBoundary): the keys, the capture's state machine, the mode and
// the list, the armed flag. `device` is the game's device; nowMs is GetTickCount64().
void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept;

// Treated draws since the last call (the cull's 5 s summary reports it, so "matched nothing" is visible).
uint64_t concoursePaneTakeTreated() noexcept;
// The mode ("off", "tint", "mirror", "grey") and the number of panes in the list, for the summary.
const char* concoursePaneModeName() noexcept;
uint32_t concoursePaneSlotCount() noexcept;
// A mode other than off with a list, on foot in social space: the draws are being treated.
bool concoursePaneTreating() noexcept;

}  // namespace edvr
