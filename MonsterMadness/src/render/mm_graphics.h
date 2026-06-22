// Native-mode "minimal command consumer" graphics system for Monster Madness.
//
// Instead of fully nulling the GPU (which hangs the guest after the first swap
// because nothing consumes the ring buffer), we provide a GraphicsSystem whose
// CommandProcessor drains the ring and performs all the bookkeeping the guest
// waits on (read-pointer write-back, fence/EVENT_WRITE packets, completion
// interrupts) but stubs the actual rendering. The game then runs frame-by-frame
// (black window) and our D3D hooks can take over rendering incrementally.
//
// See docs/RENDERER_MAPPING.md.

#pragma once

#include <memory>

namespace rex::system {
class IGraphicsSystem;
}

namespace mm {

// Builds the null-consumer graphics system to assign to RuntimeConfig::graphics.
std::unique_ptr<rex::system::IGraphicsSystem> CreateNullConsumerGraphicsSystem();

}  // namespace mm
