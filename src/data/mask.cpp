#include "data/mask.hpp"

#include <cmath>
#include <algorithm>

namespace vc {

MaskVolume::Block* MaskVolume::blockFor(int iz, int iy, int ix, bool create)
{
    ChunkId id{0, iz, iy, ix};
    auto it = store_.find(id);
    if (it != store_.end()) return it->second.get();
    if (!create) return nullptr;
    auto b = std::make_unique<Block>();
    b->fill(0);
    Block* p = b.get();
    store_.emplace(id, std::move(b));
    return p;
}

void MaskVolume::paintSphere(Vec3f c, float radius, std::uint8_t value)
{
    int r = std::max(0, int(std::ceil(radius)));
    int cx = int(c[0] + 0.5f), cy = int(c[1] + 0.5f), cz = int(c[2] + 0.5f);
    int x0 = std::max(0, cx - r), x1 = std::min(shape_[2] - 1, cx + r);
    int y0 = std::max(0, cy - r), y1 = std::min(shape_[1] - 1, cy + r);
    int z0 = std::max(0, cz - r), z1 = std::min(shape_[0] - 1, cz + r);
    if (x1 < x0 || y1 < y0 || z1 < z0) return;
    const float r2 = radius * radius;

    std::lock_guard lk(mtx_);
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float dx = x - c[0], dy = y - c[1], dz = z - c[2];
                if (dx*dx + dy*dy + dz*dz > r2) continue;
                Block* b = blockFor(z/kChunk, y/kChunk, x/kChunk, true);
                (*b)[(std::size_t(z%kChunk) * kChunk + y%kChunk) * kChunk + x%kChunk] = value;
            }
    dirty_ = true;
}

bool MaskVolume::at(int x, int y, int z) const
{
    if (x < 0 || y < 0 || z < 0 || z >= shape_[0] || y >= shape_[1] || x >= shape_[2]) return false;
    std::lock_guard lk(mtx_);
    auto it = store_.find(ChunkId{0, z/kChunk, y/kChunk, x/kChunk});
    if (it == store_.end()) return false;
    return (*it->second)[(std::size_t(z%kChunk) * kChunk + y%kChunk) * kChunk + x%kChunk] != 0;
}

const std::uint8_t* MaskVolume::chunkData(int iz, int iy, int ix) const
{
    std::lock_guard lk(mtx_);
    auto it = store_.find(ChunkId{0, iz, iy, ix});
    return it == store_.end() ? nullptr : it->second->data();
}

}  // namespace vc
