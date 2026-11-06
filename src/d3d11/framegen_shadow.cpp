// The frame-generation shadow scorer: see framegen_shadow.h.
#include "framegen_shadow.h"
#include "../common/framegen_shadow_report.h"
#include "../common/log.h"

#include <atomic>

namespace edvr {
namespace {

std::atomic<bool> g_on{false};
std::atomic<bool> g_told{false};   // the armed line has been said since the key last went on

}  // namespace

void frameGenShadowConfigure(bool on) {
    g_on.store(on);
    if (!on) g_told.store(false);
}

bool frameGenShadowOn() { return g_on.load(); }

void frameGenShadowEyeFrame(uint32_t /*eye*/, uint32_t renderW, uint32_t renderH) {
    if (!g_on.load()) return;
    if (!g_told.exchange(true)) {
        const std::string line = formatFrameGenShadowArmed((renderW + 3u) / 4u, (renderH + 3u) / 4u);
        Log::get().note("%s", line.c_str());
    }
}

}  // namespace edvr
