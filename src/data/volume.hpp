#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "util/math.hpp"

struct s3_client;  // libs3

namespace vc {

// volume-commander reads ONE format: c3d-sharded OME-zarr v3 on S3.
//   uint8, C-order; inner chunk 256^3 c3d ("C3DC"); shard 4096^3 => 16^3 inner
//   chunks/shard; shard = [index | chunk0(4k-aligned) | ...]; index =
//   4096*(offset:u64,nbytes:u64) LE at shard START; levels = subdirs "0",..
//   probed via "<n>/zarr.json"; scale = 2^level.
inline constexpr int kChunk = 256;                       // decode atom
inline constexpr int kChunkVox = kChunk * kChunk * kChunk;
inline constexpr int kShardChunks = 16;                  // inner chunks per shard axis
inline constexpr int kInnerPerShard = kShardChunks * kShardChunks * kShardChunks;

inline constexpr int kBlock = 16;                        // CACHE granularity (VC3D model)
inline constexpr int kBlockVox = kBlock * kBlock * kBlock;            // 4096 B
inline constexpr int kBlocksPerChunkAxis = kChunk / kBlock;          // 16
inline constexpr int kBlocksPerChunk = kBlocksPerChunkAxis * kBlocksPerChunkAxis * kBlocksPerChunkAxis;  // 4096

struct LevelMeta {
    std::string path;
    std::array<int, 3> shape{};
    float scale = 1.0f;
    std::array<int, 3> chunksPerAxis{};
};

// A 16^3 cached block. data==nullptr would never be stored; absent regions
// store a shared all-zero block so the sampler reads 0 without a branch.
struct Block {
    std::array<std::uint8_t, kBlockVox> v;
};

// Block coordinates (level + 16-voxel block grid).
struct BlockId {
    int level, bz, by, bx;
    bool operator==(const BlockId&) const noexcept = default;
};
struct BlockIdHash {
    std::size_t operator()(const BlockId& k) const noexcept {
        std::uint64_t h = (std::uint64_t(unsigned(k.level)) << 57)
                        ^ (std::uint64_t(unsigned(k.bz)) << 38)
                        ^ (std::uint64_t(unsigned(k.by)) << 19)
                        ^  std::uint64_t(unsigned(k.bx));
        return std::size_t(h * 0x9E3779B97F4A7C15ULL);
    }
};
// Chunk coordinates (decode unit) for the fetch queue / disk cache.
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

// Async, thread-safe volume reader. The GUI thread NEVER blocks: block()
// returns a resident 16^3 block or nullptr (and queues the parent chunk for
// async S3 fetch + c3d decode on the IOPool). A decoded 256^3 chunk is split
// into 4096 blocks and inserted into the block LRU; the chunk bytes are also
// written to a disk cache so restarts skip S3.
class Volume {
public:
    static std::shared_ptr<Volume> open(const std::string& url, int ioThreads = 6);
    ~Volume();

    int numLevels() const noexcept { return int(levels_.size()); }
    std::array<int, 3> shape(int level) const noexcept { return levels_[level].shape; }
    float scale(int level) const noexcept { return levels_[level].scale; }

    // Non-blocking. Resident -> block ptr; else nullptr + parent chunk queued.
    const Block* block(int level, int bz, int by, int bx);

    void setChunkReady(std::function<void()> cb) { onReady_ = std::move(cb); }
    void setCacheBudgetBytes(std::size_t b) { budget_ = b; }

private:
    Volume() = default;
    void worker();
    void fetchDecodeChunk(const ChunkId&);           // S3 -> decode -> split -> insert blocks
    std::vector<std::uint8_t> getRange(const std::string& key, std::uint64_t off, std::uint64_t len);
    void insertChunkBlocks(const ChunkId&, const std::uint8_t* vox256, bool present);
    void evictLocked();

    std::string diskPath(const ChunkId&) const;
    bool diskLoad(const ChunkId&, std::vector<std::uint8_t>& out256, bool& present);
    void diskStore(const ChunkId&, const std::uint8_t* vox256, bool present);

    std::string prefix_, cacheKey_;
    std::vector<LevelMeta> levels_;
    s3_client* s3_ = nullptr;

    // Block RAM cache (LRU).
    std::mutex mtx_;
    std::unordered_map<BlockId, std::list<BlockId>::iterator, BlockIdHash> map_;
    std::list<BlockId> lru_;
    std::unordered_map<BlockId, std::shared_ptr<const Block>, BlockIdHash> store_;
    std::shared_ptr<const Block> zeroBlock_;     // shared all-zero block for absent regions
    std::unordered_set<ChunkId, ChunkIdHash> haveChunk_;   // chunks already split into cache
    std::size_t bytes_ = 0;
    std::size_t budget_ = std::size_t(12) << 30;

    // IOPool: per-level chunk queue + dedup.
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
