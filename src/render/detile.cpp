// See detile.h.

#include "render/detile.h"

#include <rex/graphics/pipeline/texture/util.h>

namespace mm {

void DetileTexture8888(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height,
                       uint32_t pitch) {
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* dstRow = dst + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      // GetTiledOffset2D returns a byte offset (bytes-per-block-log2 = 2 -> 4 bytes).
      const int32_t srcOff =
          rex::graphics::texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, 2);
      const uint8_t* s = src + srcOff;
      uint8_t* d = dstRow + size_t(x) * 4;
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = s[3];
    }
  }
}

}  // namespace mm
