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
// Hardcoded layout (vc_zarr_recompress output):
//   uint8, C-order; inner chunk 256^3 c3d ("C3DC"); shard 4096^3 => 16^3 inner
//   chunks/shard; shard = [index | chunk0(4k-aligned) | ...]; index =
//   4096*(offset:u64,nbytes:u64) LE at shard START; levels = subdirs "0",..
//   probed via "<n>/zarr.json"; scale = 2^level.
inline constexpr int kChunk = 256;
inline constexpr int kChunkVox = kChunk * kChunk * kChunk;
inline constexpr int kShardChunks = 16;
inline constexpr int kInnerPerShard = kShardChunks * kShardChunks * kShardChunks;

struct LevelMeta {
    std::string path;
    std::array<int, 3> shape{};
    float scale = 1.0f;
    std::array<int, 3> chunksPerAxis{};
};

struct Chunk {
    std::vector<std::uint8_t> vox;   // kChunkVox; all-0 if absent
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

// Async, thread-safe volume reader. The GUI thread NEVER blocks here:
// chunk() returns a cached chunk or nullptr (and queues an IOPool fetch).
// All S3 IO + c3d decode runs on the IOPool worker threads; decoded chunks
// land in a RAM LRU (and a disk cache). onChunkReady fires when one arrives.
class Volume {
public:
    static std::shared_ptr<Volume> open(const std::string& url, int ioThreads = 6);
    ~Volume();

    int numLevels() const noexcept { return int(levels_.size()); }
    std::array<int, 3> shape(int level) const noexcept { return levels_[level].shape; }
    float scale(int level) const noexcept { return levels_[level].scale; }

    // Non-blocking. Cached -> chunk; else nullptr + async fetch queued.
    std::shared_ptr<const Chunk> chunk(int level, int iz, int iy, int ix);

    // Fired (on an IOPool thread) when a queued chunk finishes decoding.
    void setChunkReady(std::function<void()> cb) { onReady_ = std::move(cb); }

    void setCacheBudgetBytes(std::size_t b) { budget_ = b; }

private:
    Volume() = default;
    std::shared_ptr<const Chunk> fetchDecode(const ChunkId&);
    std::vector<std::uint8_t> getRange(const std::string& key, std::uint64_t off, std::uint64_t len);
    void worker();
    void insert(const ChunkId&, std::shared_ptr<const Chunk>);
    void evictLocked();

    // disk cache
    std::string diskPath(const ChunkId&) const;
    std::shared_ptr<const Chunk> diskLoad(const ChunkId&);
    void diskStore(const ChunkId&, const Chunk&);

    std::string prefix_;
    std::string cacheKey_;        // hash of url for disk cache dir
    std::vector<LevelMeta> levels_;
    s3_client* s3_ = nullptr;

    // RAM cache
    std::mutex mtx_;
    std::unordered_map<ChunkId, std::list<ChunkId>::iterator, ChunkIdHash> map_;
    std::list<ChunkId> lru_;
    std::unordered_map<ChunkId, std::shared_ptr<const Chunk>, ChunkIdHash> store_;
    std::size_t bytes_ = 0;
    std::size_t budget_ = std::size_t(12) << 30;

    // IOPool: level-priority queue + dedup of in-flight/queued ids.
    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::array<std::deque<ChunkId>, 16> queue_;   // per-level, coarse drained first
    std::unordered_set<ChunkId, ChunkIdHash> inflight_;
    bool stop_ = false;
    std::vector<std::jthread> workers_;
    std::function<void()> onReady_;

    ChunkId popLocked();
    bool hasWork() const;
};

}  // namespace vc
