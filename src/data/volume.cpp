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

    s3_config cfg{};
    s3_credentials_load("default", &cfg.creds);   // anonymous fallback on failure
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
    workers_.clear();   // jthread joins
    if (s3_) s3_client_free(s3_);
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

std::shared_ptr<const Chunk> Volume::fetchDecode(const ChunkId& id)
{
    if (auto d = diskLoad(id)) return d;

    const auto& lm = levels_[id.level];
    int sz = id.iz / kShardChunks, sy = id.iy / kShardChunks, sx = id.ix / kShardChunks;
    int lz = id.iz % kShardChunks, ly = id.iy % kShardChunks, lx = id.ix % kShardChunks;
    std::uint64_t linear = (std::uint64_t(lz) * kShardChunks + ly) * kShardChunks + lx;
    std::string shard = prefix_ + "/" + lm.path + "/c/"
                      + std::to_string(sz) + "/" + std::to_string(sy) + "/" + std::to_string(sx);

    auto zero = std::make_shared<Chunk>();
    zero->vox.assign(kChunkVox, 0);

    auto idx = getRange(shard, linear * 16, 16);
    if (idx.size() < 16) { diskStore(id, *zero); return zero; }
    std::uint64_t off, nb;
    std::memcpy(&off, idx.data(), 8);
    std::memcpy(&nb, idx.data() + 8, 8);
    if (off == ~0ull || nb == 0) { diskStore(id, *zero); return zero; }

    auto comp = getRange(shard, off, nb);
    if (comp.size() != nb) return zero;   // transient; don't poison disk cache

    auto c = std::make_shared<Chunk>();
    c->vox.assign(kChunkVox, 0);
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(comp.data()), comp.size());
    if (utils::is_c3d_compressed(in)) {
        utils::C3dCodecParams p; p.skip_denoise = true;
        auto dec = utils::c3d_decode(in, kChunkVox, p);
        if (dec.size() == kChunkVox) std::memcpy(c->vox.data(), dec.data(), kChunkVox);
    } else {
        std::memcpy(c->vox.data(), comp.data(), std::min<std::size_t>(comp.size(), kChunkVox));
    }
    c->present = true;
    diskStore(id, *c);
    return c;
}

std::shared_ptr<const Chunk> Volume::chunk(int level, int iz, int iy, int ix)
{
    ChunkId id{level, iz, iy, ix};
    {
        std::lock_guard lk(mtx_);
        if (auto it = store_.find(id); it != store_.end()) {
            lru_.splice(lru_.begin(), lru_, map_[id]);
            return it->second;
        }
    }
    // not resident: queue async fetch (deduped) and return nullptr now.
    {
        std::lock_guard lk(qmtx_);
        if (!inflight_.contains(id)) {
            inflight_.insert(id);
            queue_[std::clamp(level, 0, 15)].push_back(id);
        }
    }
    qcv_.notify_one();
    return nullptr;
}

bool Volume::hasWork() const
{
    for (auto& q : queue_) if (!q.empty()) return true;
    return false;
}

ChunkId Volume::popLocked()
{
    // coarsest non-empty level first => low-res streams in ahead of high-res.
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
        auto c = fetchDecode(id);
        insert(id, c);
        {
            std::lock_guard lk(qmtx_);
            inflight_.erase(id);
        }
        if (onReady_) onReady_();
    }
}

void Volume::insert(const ChunkId& id, std::shared_ptr<const Chunk> c)
{
    std::lock_guard lk(mtx_);
    if (store_.contains(id)) return;
    lru_.push_front(id);
    map_[id] = lru_.begin();
    store_[id] = std::move(c);
    bytes_ += kChunkVox;
    evictLocked();
}

void Volume::evictLocked()
{
    while (bytes_ > budget_ && !lru_.empty()) {
        ChunkId old = lru_.back();
        lru_.pop_back();
        store_.erase(old);
        map_.erase(old);
        bytes_ -= kChunkVox;
    }
}

std::string Volume::diskPath(const ChunkId& id) const
{
    return std::string(getenv("HOME") ? getenv("HOME") : ".")
         + "/.cache/volume-commander/" + cacheKey_ + "/"
         + std::to_string(id.level) + "_" + std::to_string(id.iz) + "_"
         + std::to_string(id.iy) + "_" + std::to_string(id.ix) + ".chunk";
}

std::shared_ptr<const Chunk> Volume::diskLoad(const ChunkId& id)
{
    std::ifstream f(diskPath(id), std::ios::binary);
    if (!f) return nullptr;
    char tag;
    f.read(&tag, 1);
    auto c = std::make_shared<Chunk>();
    c->vox.assign(kChunkVox, 0);
    if (tag) { f.read(reinterpret_cast<char*>(c->vox.data()), kChunkVox); c->present = true; }
    if (!f && tag) return nullptr;
    return c;
}

void Volume::diskStore(const ChunkId& id, const Chunk& c)
{
    namespace fs = std::filesystem;
    auto p = fs::path(diskPath(id));
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return;
    char tag = c.present ? 1 : 0;
    f.write(&tag, 1);
    if (c.present) f.write(reinterpret_cast<const char*>(c.vox.data()), kChunkVox);
}

}  // namespace vc
