#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/math.hpp"

struct s3_client;  // libs3

namespace vc {

// volume-commander only reads ONE format: c3d-sharded OME-zarr v3 on S3.
// Hardcoded layout (the vc_zarr_recompress output):
//   - uint8, C-order
//   - inner chunk: 256^3, c3d-compressed ("C3DC" magic)
//   - shard:       4096^3  => 16x16x16 = 4096 inner chunks per shard
//   - shard file:  [index | chunk0(4k-aligned) | chunk1 | ...]
//                  index = 4096 * (offset:u64, nbytes:u64) little-endian, C-order
//   - pyramid:     levels are subdirs "0","1",... — DETECTED by probing for
//                  each "<n>/zarr.json"; scale assumed 2^level. No OME/.zattrs parse.
inline constexpr int kChunk = 256;        // inner chunk side
inline constexpr int kChunkVox = kChunk * kChunk * kChunk;
inline constexpr int kShardChunks = 16;   // inner chunks per shard axis (4096/256)
inline constexpr int kInnerPerShard = kShardChunks * kShardChunks * kShardChunks;  // 4096
inline constexpr std::uint64_t kIndexBytes = std::uint64_t(kInnerPerShard) * 16;

struct LevelMeta {
    std::string path;            // dataset path component, e.g. "0"
    std::array<int, 3> shape{};  // {z, y, x} in voxels
    float scale = 1.0f;          // downsample factor vs level 0 (e.g. 1,2,4,...)
    std::array<int, 3> chunksPerAxis{};  // ceil(shape / 256)
};

// A decoded 256^3 chunk (or a smaller edge chunk, always allocated 256^3).
struct Chunk {
    std::vector<std::uint8_t> vox;   // kChunkVox, 0 if absent
    bool present = false;
};

struct ChunkId {
    int level, iz, iy, ix;
    bool operator==(const ChunkId&) const noexcept = default;
};
struct ChunkIdHash {
    std::size_t operator()(const ChunkId& k) const noexcept {
        std::uint64_t h = (std::uint64_t(unsigned(k.level)) << 56)
                        ^ (std::uint64_t(unsigned(k.iz)) << 38)
                        ^ (std::uint64_t(unsigned(k.iy)) << 19)
                        ^  std::uint64_t(unsigned(k.ix));
        return std::size_t(h * 0x9E3779B97F4A7C15ULL);
    }
};

// Reads + decodes + caches chunks from one zarr volume on S3. Thread-safe.
class Volume {
public:
    // url like "s3://philodemos/forrest/.../volume.zarr"
    static std::shared_ptr<Volume> open(const std::string& url);
    ~Volume();

    int numLevels() const noexcept { return int(levels_.size()); }
    std::array<int, 3> shape(int level) const noexcept { return levels_[level].shape; }
    float scale(int level) const noexcept { return levels_[level].scale; }

    // Decoded 256^3 chunk for (level, chunk indices). Cached; blocks on miss.
    // Returns nullptr only on hard error; absent chunks return a zero Chunk.
    std::shared_ptr<const Chunk> chunk(int level, int iz, int iy, int ix);

    // Queue chunks for background decode (best-effort; no-op if already hot).
    void prefetch(int level, const std::vector<std::array<int,3>>& chunks);

    void setCacheBudgetBytes(std::size_t b) { budget_ = b; }

private:
    Volume() = default;
    std::shared_ptr<const Chunk> fetchDecode(const ChunkId&);
    std::vector<std::uint8_t> getRange(const std::string& key, std::uint64_t off, std::uint64_t len);

    std::string bucket_, prefix_;     // parsed from url
    std::vector<LevelMeta> levels_;
    s3_client* s3_ = nullptr;

    std::mutex mtx_;
    std::unordered_map<ChunkId, std::list<ChunkId>::iterator, ChunkIdHash> map_;
    std::list<ChunkId> lru_;          // front = newest
    std::unordered_map<ChunkId, std::shared_ptr<const Chunk>, ChunkIdHash> store_;
    std::size_t bytes_ = 0;
    std::size_t budget_ = std::size_t(8) << 30;
    void evictLocked();
};

}  // namespace vc
