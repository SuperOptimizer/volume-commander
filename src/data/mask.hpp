#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "util/math.hpp"
#include "data/volume.hpp"   // ChunkId, ChunkIdHash, kChunk

namespace vc {

// Writable binary label volume, sized to a source volume's level-0 shape.
// Sparse: only painted 256^3 chunks are allocated (1 byte/voxel, 0 or 255).
// Same chunk addressing as Volume so the renderer samples it identically.
class MaskVolume {
public:
    explicit MaskVolume(std::array<int, 3> shape0) : shape_(shape0) {}

    std::array<int, 3> shape() const noexcept { return shape_; }

    // Stamp a solid sphere of `value` (0/255) centered at world voxel `c`.
    void paintSphere(Vec3f c, float radius, std::uint8_t value);

    // Test one voxel (level 0). Cheap: returns false for unallocated chunks.
    bool at(int x, int y, int z) const;

    // Allocated chunk for the renderer (nullptr if never painted).
    const std::uint8_t* chunkData(int iz, int iy, int ix) const;

    bool dirty() const noexcept { return dirty_; }
    void clearDirty() noexcept { dirty_ = false; }
    std::size_t paintedChunks() const { std::lock_guard lk(mtx_); return store_.size(); }

    // For serialization (#8 save): iterate allocated chunks.
    template <class F> void forEachChunk(F&& f) const {
        std::lock_guard lk(mtx_);
        for (auto& [id, c] : store_) f(id, c->data());
    }

private:
    using Block = std::array<std::uint8_t, std::size_t(kChunk) * kChunk * kChunk>;
    Block* blockFor(int iz, int iy, int ix, bool create);

    std::array<int, 3> shape_;  // {z, y, x}
    mutable std::mutex mtx_;
    std::unordered_map<ChunkId, std::unique_ptr<Block>, ChunkIdHash> store_;
    bool dirty_ = false;
};

}  // namespace vc
