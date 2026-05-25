#include "data/volume.hpp"

#include <cstring>
#include <charconv>
#include <fstream>
#include <filesystem>
#include <print>

extern "C" {
#include <libs3.h>
}
#include <utils/c3d_codec.hpp>

namespace vc {
namespace {

std::optional<std::vector<long long>> jsonIntArray(std::string_view s, std::string_view key)
{
    auto p = s.find(key);
    if (p == s.npos) return std::nullopt;
    p = s.find('[', p); if (p == s.npos) return std::nullopt;
    auto e = s.find(']', p); if (e == s.npos) return std::nullopt;
    std::vector<long long> out;
    for (std::size_t i = p + 1; i < e;) {
        while (i < e && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\t')) ++i;
        if (i >= e) break;
        long long v = 0;
        auto [np, ec] = std::from_chars(s.data() + i, s.data() + e, v);
        if (ec != std::errc{}) break;
        out.push_back(v); i = std::size_t(np - s.data());
    }
    return out;
}
std::uint64_t fnv(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s) { h ^= std::uint8_t(c); h *= 1099511628211ull; }
    return h;
}

}  // namespace

std::shared_ptr<Volume> Volume::open(const std::string& url, int ioThreads)
{
    auto vol = std::shared_ptr<Volume>(new Volume);
    vol->prefix_ = url;
    while (!vol->prefix_.empty() && vol->prefix_.back() == '/') vol->prefix_.pop_back();
    vol->cacheKey_ = std::to_string(fnv(vol->prefix_));

    // lock-free table sized for ~12 GB of 4 KB blocks (~3M) -> 8M slots (load <0.5)
    constexpr std::size_t kSlots = std::size_t(1) << 23;   // 8,388,608
    vol->slots_ = std::vector<Slot>(kSlots);
    vol->slotMask_ = kSlots - 1;
    vol->arena_.emplace_back(1);                           // slab 0, one block = zero block
    std::memset(vol->arena_.back()[0].v.data(), 0, kBlockVox);
    vol->zeroBlock_ = &vol->arena_.back()[0];

    vol->local_ = !vol->prefix_.starts_with("s3://");
    if (!vol->local_) {
        s3_config cfg{};
        s3_credentials_load("default", &cfg.creds);
        vol->s3_ = s3_client_new(&cfg);
    }

    for (int lvl = 0; lvl < 16; ++lvl) {
        auto meta = vol->getAll(vol->prefix_ + "/" + std::to_string(lvl) + "/zarr.json");
        if (meta.empty()) break;
        std::string_view body(reinterpret_cast<char*>(meta.data()), meta.size());
        auto shp = jsonIntArray(body, "\"shape\"");
        if (!shp || shp->size() < 3) break;
        LevelMeta m;
        m.path = std::to_string(lvl);
        m.shape = {int((*shp)[0]), int((*shp)[1]), int((*shp)[2])};
        m.scale = float(1u << lvl);
        for (int d = 0; d < 3; ++d) m.chunksPerAxis[d] = (m.shape[d] + kChunk - 1) / kChunk;
        vol->levels_.push_back(m);
    }
    if (vol->levels_.empty()) { std::println(stderr, "Volume::open: no levels under {}", url); return nullptr; }

    for (int i = 0; i < ioThreads; ++i)
        vol->workers_.emplace_back([v = vol.get()] { v->worker(); });
    return vol;
}

Volume::~Volume()
{
    { std::lock_guard lk(qmtx_); stop_ = true; }
    qcv_.notify_all();
    workers_.clear();
    if (s3_) s3_client_free(s3_);
}

// ---- LOCK-FREE block lookup ----------------------------------------------
const Block* Volume::block(int level, int bz, int by, int bx) noexcept
{
    const std::uint64_t key = blockKey(level, bz, by, bx);
    std::size_t i = mix(key) & slotMask_;
    for (std::size_t probe = 0; probe < 64; ++probe) {       // bounded linear probe
        const Slot& s = slots_[i];
        std::uint64_t k = s.key.load(std::memory_order_acquire);
        if (k == key) return s.ptr.load(std::memory_order_acquire);
        if (k == 0) break;                                    // empty -> not present
        i = (i + 1) & slotMask_;
    }
    // miss: queue the parent chunk (one decode publishes 4096 blocks)
    queueChunk(ChunkId{level, bz / kBlocksPerChunkAxis, by / kBlocksPerChunkAxis, bx / kBlocksPerChunkAxis});
    return nullptr;
}

void Volume::queueChunk(const ChunkId& cid)
{
    { std::lock_guard lk(haveMtx_); if (haveChunk_.contains(cid)) return; }
    {
        std::lock_guard lk(qmtx_);
        if (inflight_.contains(cid)) return;
        inflight_.insert(cid);
        queue_[std::clamp(cid.level, 0, 15)].push_back(cid);
    }
    qcv_.notify_one();
}

bool Volume::hasWork() const { for (auto& q : queue_) if (!q.empty()) return true; return false; }
ChunkId Volume::popLocked()
{
    for (int l = 15; l >= 0; --l)
        if (!queue_[l].empty()) { ChunkId id = queue_[l].front(); queue_[l].pop_front(); return id; }
    return {-1, 0, 0, 0};
}

void Volume::worker()
{
    for (;;) {
        ChunkId id;
        {
            std::unique_lock lk(qmtx_);
            qcv_.wait(lk, [&] { return stop_ || hasWork(); });
            if (stop_) return;
            id = popLocked();
            if (id.level < 0) continue;
        }
        fetchDecodeChunk(id);
        { std::lock_guard lk(qmtx_); inflight_.erase(id); }
        if (onReady_) onReady_();
    }
}

std::vector<std::uint8_t> Volume::getAll(const std::string& key)
{
    if (local_) {
        std::ifstream f(key, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::size_t n = std::size_t(f.tellg()); f.seekg(0);
        std::vector<std::uint8_t> out(n);
        f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n));
        if (!f) return {};
        return out;
    }
    s3_response r{};
    std::vector<std::uint8_t> out;
    if (s3_get(s3_, key.c_str(), &r) == S3_OK && r.status == 200 && r.body)
        out.assign(r.body, r.body + r.body_len);
    s3_response_free(&r);
    return out;
}

std::vector<std::uint8_t> Volume::getRange(const std::string& key, std::uint64_t off, std::uint64_t len)
{
    if (local_) {
        std::ifstream f(key, std::ios::binary);
        if (!f) return {};
        f.seekg(std::streamoff(off));
        std::vector<std::uint8_t> out(len);
        f.read(reinterpret_cast<char*>(out.data()), std::streamsize(len));
        out.resize(std::size_t(f.gcount()));
        return out;
    }
    s3_response r{};
    auto rc = s3_get_range(s3_, key.c_str(), off, len, &r);
    std::vector<std::uint8_t> out;
    if (rc == S3_OK && (r.status == 206 || r.status == 200) && r.body)
        out.assign(r.body, r.body + r.body_len);
    s3_response_free(&r);
    return out;
}

void Volume::fetchDecodeChunk(const ChunkId& id)
{
    std::vector<std::uint8_t> vox; bool present = false;
    if (diskLoad(id, vox, present)) { publishChunk(id, present ? vox.data() : nullptr, present); return; }

    const auto& lm = levels_[id.level];
    int sz = id.iz / kShardChunks, sy = id.iy / kShardChunks, sx = id.ix / kShardChunks;
    int lz = id.iz % kShardChunks, ly = id.iy % kShardChunks, lx = id.ix % kShardChunks;
    std::uint64_t linear = (std::uint64_t(lz) * kShardChunks + ly) * kShardChunks + lx;
    std::string shard = prefix_ + "/" + lm.path + "/c/"
                      + std::to_string(sz) + "/" + std::to_string(sy) + "/" + std::to_string(sx);

    auto idx = getRange(shard, linear * 16, 16);
    if (idx.size() < 16) { diskStore(id, nullptr, false); publishChunk(id, nullptr, false); return; }
    std::uint64_t off, nb;
    std::memcpy(&off, idx.data(), 8); std::memcpy(&nb, idx.data() + 8, 8);
    if (off == ~0ull || nb == 0) { diskStore(id, nullptr, false); publishChunk(id, nullptr, false); return; }

    auto comp = getRange(shard, off, nb);
    if (comp.size() != nb) { publishChunk(id, nullptr, false); return; }   // transient

    std::vector<std::uint8_t> out(kChunkVox, 0);
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(comp.data()), comp.size());
    if (utils::is_c3d_compressed(in)) {
        utils::C3dCodecParams p; p.skip_denoise = true;
        auto dec = utils::c3d_decode(in, kChunkVox, p);
        if (dec.size() == kChunkVox) std::memcpy(out.data(), dec.data(), kChunkVox);
    } else {
        std::memcpy(out.data(), comp.data(), std::min<std::size_t>(comp.size(), kChunkVox));
    }
    diskStore(id, out.data(), true);
    publishChunk(id, out.data(), true);
}

// Split a decoded 256^3 chunk into 4096 blocks and publish each into the
// lock-free table (release store). Absent chunk -> publish the shared zero
// block for every slot so readers get 0 without ever queueing again.
void Volume::publishChunk(const ChunkId& id, const std::uint8_t* vox, bool present)
{
    { std::lock_guard lk(haveMtx_); if (!haveChunk_.insert(id).second) return; }

    const int b0z = id.iz * kBlocksPerChunkAxis;
    const int b0y = id.iy * kBlocksPerChunkAxis;
    const int b0x = id.ix * kBlocksPerChunkAxis;

    // One slab of 4096 blocks for the whole chunk, allocated under a SINGLE
    // lock (was one lock + one zero-init per block = 4096x). Blocks are filled
    // by memcpy below (every byte written), so no pre-zeroing needed.
    std::vector<Block>* slab = nullptr;
    if (present) {
        std::lock_guard lk(arenaMtx_);
        arena_.emplace_back(kBlocksPerChunk);
        slab = &arena_.back();
    }
    // Copy pass: scan the 256^3 source SEQUENTIALLY (storage order) and scatter
    // each 16-byte x-run into its block. The old order looped blocks and read
    // strided 16^3 sub-cubes from the chunk — cache-hostile (cachegrind: ~85%
    // of all D1/LL read-misses). Reading the source linearly turns those into
    // streaming hits; only the block-local writes (4 KB each, hot) scatter.
    auto blockIndex = [](int bz, int by, int bx) {
        return (bz * kBlocksPerChunkAxis + by) * kBlocksPerChunkAxis + bx;
    };
    if (present) {
        for (int z = 0; z < kChunk; ++z) {
            int bz = z >> 4, lz = z & 15;
            for (int y = 0; y < kChunk; ++y) {
                int by = y >> 4, ly = y & 15;
                const std::uint8_t* srow = vox + (std::size_t(z) * kChunk + y) * kChunk;
                Block* base = &(*slab)[blockIndex(bz, by, 0)];
                for (int bx = 0; bx < kBlocksPerChunkAxis; ++bx) {
                    std::memcpy(base[bx].v.data() + (std::size_t(lz) * kBlock + ly) * kBlock,
                                srow + bx * kBlock, kBlock);
                }
            }
        }
    }

    for (int bz = 0; bz < kBlocksPerChunkAxis; ++bz)
      for (int by = 0; by < kBlocksPerChunkAxis; ++by)
        for (int bx = 0; bx < kBlocksPerChunkAxis; ++bx) {
            const Block* blk = present ? &(*slab)[blockIndex(bz, by, bx)] : zeroBlock_;
            // insert into lock-free table: claim an empty slot via CAS on key.
            std::uint64_t key = blockKey(id.level, b0z + bz, b0y + by, b0x + bx);
            std::size_t i = mix(key) & slotMask_;
            for (std::size_t probe = 0; probe < 64; ++probe) {
                Slot& s = slots_[i];
                std::uint64_t k = s.key.load(std::memory_order_acquire);
                if (k == key) break;                       // already published
                if (k == 0) {
                    std::uint64_t expected = 0;
                    if (s.key.compare_exchange_strong(expected, key,
                            std::memory_order_acq_rel, std::memory_order_acquire)) {
                        s.ptr.store(blk, std::memory_order_release);
                        break;
                    }
                    if (expected == key) break;            // someone else published same
                }
                i = (i + 1) & slotMask_;
            }
        }
}

// ---- disk cache (one file per 256^3 chunk) --------------------------------
std::string Volume::diskPath(const ChunkId& id) const
{
    return std::string(getenv("HOME") ? getenv("HOME") : ".")
         + "/.cache/volume-commander/" + cacheKey_ + "/"
         + std::to_string(id.level) + "_" + std::to_string(id.iz) + "_"
         + std::to_string(id.iy) + "_" + std::to_string(id.ix) + ".chunk";
}
bool Volume::diskLoad(const ChunkId& id, std::vector<std::uint8_t>& out, bool& present)
{
    std::ifstream f(diskPath(id), std::ios::binary);
    if (!f) return false;
    char tag = 0; f.read(&tag, 1);
    present = tag != 0;
    if (present) { out.assign(kChunkVox, 0); f.read(reinterpret_cast<char*>(out.data()), kChunkVox); if (!f) return false; }
    return true;
}
void Volume::diskStore(const ChunkId& id, const std::uint8_t* vox, bool present)
{
    namespace fs = std::filesystem;
    auto p = fs::path(diskPath(id));
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return;
    char tag = present ? 1 : 0; f.write(&tag, 1);
    if (present && vox) f.write(reinterpret_cast<const char*>(vox), kChunkVox);
}

}  // namespace vc
