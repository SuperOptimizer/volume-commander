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
    p = s.find('[', p);
    if (p == s.npos) return std::nullopt;
    auto e = s.find(']', p);
    if (e == s.npos) return std::nullopt;
    std::vector<long long> out;
    for (std::size_t i = p + 1; i < e;) {
        while (i < e && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\t')) ++i;
        if (i >= e) break;
        long long v = 0;
        auto [np, ec] = std::from_chars(s.data() + i, s.data() + e, v);
        if (ec != std::errc{}) break;
        out.push_back(v);
        i = std::size_t(np - s.data());
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
    vol->zeroBlock_ = std::make_shared<Block>();   // all zero

    s3_config cfg{};
    s3_credentials_load("default", &cfg.creds);
    vol->s3_ = s3_client_new(&cfg);

    for (int lvl = 0; lvl < 16; ++lvl) {
        std::string key = vol->prefix_ + "/" + std::to_string(lvl) + "/zarr.json";
        s3_response r{};
        if (s3_get(vol->s3_, key.c_str(), &r) != S3_OK || r.status != 200 || !r.body) {
            s3_response_free(&r); break;
        }
        std::string_view body(reinterpret_cast<char*>(r.body), r.body_len);
        auto shp = jsonIntArray(body, "\"shape\"");
        s3_response_free(&r);
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

// ---- block lookup (non-blocking) -----------------------------------------
const Block* Volume::block(int level, int bz, int by, int bx)
{
    BlockId id{level, bz, by, bx};
    {
        std::lock_guard lk(mtx_);
        if (auto it = store_.find(id); it != store_.end()) {
            lru_.splice(lru_.begin(), lru_, map_[id]);
            return it->second.get();
        }
    }
    // not resident -> queue the PARENT chunk (one decode fills 4096 blocks).
    ChunkId cid{level, bz / kBlocksPerChunkAxis, by / kBlocksPerChunkAxis, bx / kBlocksPerChunkAxis};
    queueChunk(cid);
    return nullptr;
}

void Volume::queueChunk(const ChunkId& cid)
{
    {
        std::lock_guard lk(mtx_);
        if (haveChunk_.contains(cid)) return;   // already split into cache
    }
    {
        std::lock_guard lk(qmtx_);
        if (inflight_.contains(cid)) return;
        inflight_.insert(cid);
        queue_[std::clamp(cid.level, 0, 15)].push_back(cid);
    }
    qcv_.notify_one();
}

// ---- IOPool worker --------------------------------------------------------
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

std::vector<std::uint8_t> Volume::getRange(const std::string& key, std::uint64_t off, std::uint64_t len)
{
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
    // disk cache first
    std::vector<std::uint8_t> vox; bool present = false;
    if (diskLoad(id, vox, present)) { insertChunkBlocks(id, present ? vox.data() : nullptr, present); return; }

    const auto& lm = levels_[id.level];
    int sz = id.iz / kShardChunks, sy = id.iy / kShardChunks, sx = id.ix / kShardChunks;
    int lz = id.iz % kShardChunks, ly = id.iy % kShardChunks, lx = id.ix % kShardChunks;
    std::uint64_t linear = (std::uint64_t(lz) * kShardChunks + ly) * kShardChunks + lx;
    std::string shard = prefix_ + "/" + lm.path + "/c/"
                      + std::to_string(sz) + "/" + std::to_string(sy) + "/" + std::to_string(sx);

    auto idx = getRange(shard, linear * 16, 16);
    if (idx.size() < 16) { diskStore(id, nullptr, false); insertChunkBlocks(id, nullptr, false); return; }
    std::uint64_t off, nb;
    std::memcpy(&off, idx.data(), 8);
    std::memcpy(&nb, idx.data() + 8, 8);
    if (off == ~0ull || nb == 0) { diskStore(id, nullptr, false); insertChunkBlocks(id, nullptr, false); return; }

    auto comp = getRange(shard, off, nb);
    if (comp.size() != nb) { insertChunkBlocks(id, nullptr, false); return; }  // transient; don't poison disk

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
    insertChunkBlocks(id, out.data(), true);
}

// Split a decoded 256^3 chunk into 16^3 blocks and insert into the LRU.
void Volume::insertChunkBlocks(const ChunkId& id, const std::uint8_t* vox, bool present)
{
    std::lock_guard lk(mtx_);
    if (haveChunk_.contains(id)) return;
    haveChunk_.insert(id);

    const int b0z = id.iz * kBlocksPerChunkAxis;
    const int b0y = id.iy * kBlocksPerChunkAxis;
    const int b0x = id.ix * kBlocksPerChunkAxis;

    for (int bz = 0; bz < kBlocksPerChunkAxis; ++bz)
      for (int by = 0; by < kBlocksPerChunkAxis; ++by)
        for (int bx = 0; bx < kBlocksPerChunkAxis; ++bx) {
            BlockId bid{id.level, b0z + bz, b0y + by, b0x + bx};
            std::shared_ptr<const Block> blk;
            if (!present) {
                blk = zeroBlock_;            // share one zero block; costs nothing
            } else {
                auto nb = std::make_shared<Block>();
                // copy 16^3 sub-cube out of the 256^3 chunk
                for (int z = 0; z < kBlock; ++z)
                  for (int y = 0; y < kBlock; ++y) {
                    const std::uint8_t* src = vox
                        + (std::size_t((bz*kBlock + z)) * kChunk + (by*kBlock + y)) * kChunk + bx*kBlock;
                    std::memcpy(nb->v.data() + (std::size_t(z)*kBlock + y)*kBlock, src, kBlock);
                  }
                blk = std::move(nb);
            }
            if (store_.contains(bid)) continue;
            lru_.push_front(bid);
            map_[bid] = lru_.begin();
            store_[bid] = std::move(blk);
            if (store_[bid] != zeroBlock_) bytes_ += kBlockVox;   // zero block shared, ~free
        }
    evictLocked();
}

void Volume::evictLocked()
{
    while (bytes_ > budget_ && !lru_.empty()) {
        BlockId old = lru_.back();
        lru_.pop_back();
        auto it = store_.find(old);
        if (it != store_.end()) {
            if (it->second != zeroBlock_) bytes_ -= kBlockVox;
            store_.erase(it);
        }
        map_.erase(old);
        // Note: parent chunk stays in haveChunk_; a re-touch re-queues + re-splits.
        // Acceptable: eviction only under memory pressure.
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
    if (present) { out.assign(kChunkVox, 0); f.read(reinterpret_cast<char*>(out.data()), kChunkVox);
                   if (!f) return false; }
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
