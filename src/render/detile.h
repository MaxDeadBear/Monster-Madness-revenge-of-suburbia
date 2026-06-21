// Xenos 2D texture detiling (isolates the SDK GetTiledOffset2D dependency).

#pragma once

#include <cstdint>

namespace mm {

// Detile a tiled 8.8.8.8 (4 bytes/texel) Xenos surface into a linear row-major
// buffer. `pitch` is the texel pitch (>= width, 32-aligned). dst must hold
// width*height*4 bytes. Copies texels as-is (no channel swap).
void DetileTexture8888(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height,
                       uint32_t pitch);

}  // namespace mm
