#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "util/math.hpp"

struct s3_client;  // libs3

namespace vc {

// c3d-sharded OME-zarr v3 on S3. uint8; inner chunk 256^3 c3d; shard 4096^3
// => 16^3 inner chunks/shard; shard = [index | chunk0(4k-aligned) | ...];
// index = 4096*(offset:u64,nbytes:u64) LE at START; levels = subdirs probed
// via "<n>/zarr.json"; scale = 2^level.
inline constexpr int kChunk = 256;
inline constexpr int kChunkVox = kChunk * kChunk * kChunk;
inline constexpr int kShardChunks = 16;
inline constexpr int kInnerPerShard = kShardChunks * kShardChunks * kShardChunks;

inline constexpr int kBlock = 16;                          // cache granularity (VC3D)
inline constexpr int kBlockVox = kBlock * kBlock * kBlock; // 4096 B
inline constexpr int kBlocksPerChunkAxis = kChunk / kBlock; // 16
inline constexpr int kBlocksPerChunk = kBlocksPerChunkAxis * kBlocksPerChunkAxis * kBlocksPerChunkAxis;

struct LevelMeta {
    std::string path;
    std::array<int, 3> shape{};
    float scale = 1.0f;
    std::array<int, 3> chunksPerAxis{};
};

struct Block { std::array<std::uint8_t, kBlockVox> v; };

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

// Lock-free block cache. Open-addressed table of (key, Block*) slots. Readers
// probe with acquire loads — NO locks, NO contention. IO workers publish
// decoded blocks with release stores. Blocks are arena-allocated and live for
// the session (immutable), so reader pointers never dangle. The render hot
// path (millions of samples) never touches a mutex.
class Volume {
public:
    static std::shared_ptr<Volume> open(const std::string& url, int ioThreads = 6);
    ~Volume();

    int numLevels() const noexcept { return int(levels_.size()); }
    std::array<int, 3> shape(int level) const noexcept { return levels_[level].shape; }
    float scale(int level) const noexcept { return levels_[level].scale; }

    // Lock-free. Resident -> block ptr; else nullptr + parent chunk queued.
    const Block* block(int level, int bz, int by, int bx) noexcept;

    void setChunkReady(std::function<void()> cb) { onReady_ = std::move(cb); }

private:
    Volume() = default;
    void worker();
    void fetchDecodeChunk(const ChunkId&);
    std::vector<std::uint8_t> getRange(const std::string& key, std::uint64_t off, std::uint64_t len);
    std::vector<std::uint8_t> getAll(const std::string& key);   // whole object/file
    void publishChunk(const ChunkId&, const std::uint8_t* vox256, bool present);
    std::string diskPath(const ChunkId&) const;
    bool diskLoad(const ChunkId&, std::vector<std::uint8_t>& out256, bool& present);
    void diskStore(const ChunkId&, const std::uint8_t* vox256, bool present);

    static std::uint64_t blockKey(int level, int bz, int by, int bx) noexcept {
        // sentinel(1)=1 | level(3) | bz(20) | by(20) | bx(20) = 64 bits, packed
        // symmetrically. Top bit always set so a valid key is never 0 (0 ==
        // empty slot). 8 levels; 20-bit axis = 1M blocks = 16M voxels/axis.
        return (std::uint64_t(1) << 63)
             | (std::uint64_t(unsigned(level) & 0x7) << 60)
             | (std::uint64_t(unsigned(bz) & 0xFFFFF) << 40)
             | (std::uint64_t(unsigned(by) & 0xFFFFF) << 20)
             |  std::uint64_t(unsigned(bx) & 0xFFFFF);
    }
    static std::uint64_t mix(std::uint64_t k) noexcept {
        k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; return k;
    }

    struct Slot {
        std::atomic<std::uint64_t> key{0};        // 0 = empty (keys are +1 packed)
        std::atomic<const Block*> ptr{nullptr};
    };

    std::string prefix_, cacheKey_;
    bool local_ = false;            // prefix_ is a filesystem path, not s3://
    std::vector<LevelMeta> levels_;
    s3_client* s3_ = nullptr;

    // lock-free slot table (power-of-2) + block arena (never freed in-session)
    std::vector<Slot> slots_;
    std::uint64_t slotMask_ = 0;
    std::deque<Block> arena_;                     // stable addresses
    std::mutex arenaMtx_;                         // only IO workers, only on publish
    const Block* zeroBlock_ = nullptr;            // shared all-zero block (absent regions)

    // chunk dedup (publish-once) — small set, only touched by IO workers
    std::mutex haveMtx_;
    std::unordered_set<ChunkId, ChunkIdHash> haveChunk_;

    // IOPool
    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::array<std::deque<ChunkId>, 16> queue_;
    std::unordered_set<ChunkId, ChunkIdHash> inflight_;
    bool stop_ = false;
    std::vector<std::jthread> workers_;
    std::function<void()> onReady_;

    void queueChunk(const ChunkId&);
    ChunkId popLocked();
    bool hasWork() const;
};

}  // namespace vc
