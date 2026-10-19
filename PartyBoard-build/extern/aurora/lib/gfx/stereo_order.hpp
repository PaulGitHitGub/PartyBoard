#pragma once

// The order the eyes' world draws go to the GPU in (gfx/common.cpp), kept
// free of Aurora's types so tools/tests can check it.
//
// A tiled GPU (the headset's Adreno) rejects hidden fragments early only
// behind depth already written (LRZ), so opaque draws are best sent front to
// back. The game sends them in its models' order. Only a run of consecutive
// sortable draws is reordered: an opaque, depth-tested, depth-writing draw of
// an object (stereo.cpp decides). Anything else stays where it is and ends
// the run: blended draws, draws outside an object (particles, sprites), and
// the game's own back-to-front pass. Within a run, draws go by the distance of
// their object from the eyes, and an object's draws stay together, in order.

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace aurora::gfx::stereo {

struct DrawOrder {
  float key = -1.f;     // the object's distance from the eyes; < 0: not in an object
  uint32_t object = 0;  // which object, in the order the game drew them
  bool sortable = false;
};

// The draws' indices in the order to draw them.
inline std::vector<uint32_t> draw_sequence(const std::vector<DrawOrder>& order, bool sortOpaque) {
  std::vector<uint32_t> sequence(order.size());
  std::iota(sequence.begin(), sequence.end(), 0u);
  if (!sortOpaque) {
    return sequence;
  }
  for (size_t start = 0; start < sequence.size();) {
    if (!order[start].sortable) {
      ++start;
      continue;
    }
    size_t end = start;
    while (end < sequence.size() && order[end].sortable) {
      ++end;
    }
    std::stable_sort(sequence.begin() + start, sequence.begin() + end, [&](uint32_t a, uint32_t b) {
      const auto& x = order[a];
      const auto& y = order[b];
      return x.key < y.key || (x.key == y.key && x.object < y.object);
    });
    start = end;
  }
  return sequence;
}

} // namespace aurora::gfx::stereo
