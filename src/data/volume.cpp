#include "data/volume.hpp"

#include <cstring>
#include <charconv>
#include <print>

extern "C" {
#include <libs3.h>
}
#include <utils/c3d_codec.hpp>

namespace vc {
namespace {

// Pull the first JSON integer array named `key` out of a small metadata blob.
// zarr.json is tiny and machine-written; a real parser would be overkill.
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

}  // namespace

std::shared_ptr<Volume> Volume::open(const std::string& url)
{
    auto vol = std::shared_ptr<Volume>(new Volume);
    vol->prefix_ = url;
    while (!vol->prefix_.empty() && vol->prefix_.back() == '/') vol->prefix_.pop_back();

    s3_config cfg{};
    s3_credentials_load("default", &cfg.creds);  // falls back to anonymous on failure
    vol->s3_ = s3_client_new(&cfg);

    // Probe for level dirs 0,1,2,... by reading "<base>/<n>/zarr.json".
    for (int lvl = 0; lvl < 16; ++lvl) {
        std::string key = vol->prefix_ + "/" + std::to_string(lvl) + "/zarr.json";
        s3_response r{};
        if (s3_get(vol->s3_, key.c_str(), &r) != S3_OK || r.status != 200 || !r.body) {
            s3_response_free(&r);
            break;
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

    if (vol->levels_.empty()) {
        std::println(stderr, "Volume::open: no levels found under {}", url);
        return nullptr;
    }
    return vol;
}

Volume::~Volume() { if (s3_) s3_client_free(s3_); }

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
    const auto& lm = levels_[id.level];
    // inner chunk -> shard + linear inner index (C-order, stride 16 per dim).
    int sz = id.iz / kShardChunks, sy = id.iy / kShardChunks, sx = id.ix / kShardChunks;
    int lz = id.iz % kShardChunks, ly = id.iy % kShardChunks, lx = id.ix % kShardChunks;
    std::uint64_t linear = (std::uint64_t(lz) * kShardChunks + ly) * kShardChunks + lx;

    std::string shard = prefix_ + "/" + lm.path + "/c/"
                      + std::to_string(sz) + "/" + std::to_string(sy) + "/" + std::to_string(sx);

    auto zero = std::make_shared<Chunk>();
    zero->vox.assign(kChunkVox, 0);

    auto idxBytes = getRange(shard, linear * 16, 16);
    if (idxBytes.size() < 16) return zero;            // missing shard -> empty
    std::uint64_t off, nbytes;
    std::memcpy(&off, idxBytes.data(), 8);
    std::memcpy(&nbytes, idxBytes.data() + 8, 8);
    if (off == ~0ull || nbytes == 0) return zero;     // missing / empty inner chunk

    auto comp = getRange(shard, off, nbytes);
    if (comp.size() != nbytes) return zero;

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
    auto c = fetchDecode(id);   // decode outside the lock
    {
        std::lock_guard lk(mtx_);
        if (auto it = store_.find(id); it != store_.end()) return it->second;  // raced
        lru_.push_front(id);
        map_[id] = lru_.begin();
        store_[id] = c;
        bytes_ += kChunkVox;
        evictLocked();
    }
    return c;
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

void Volume::prefetch(int level, const std::vector<std::array<int,3>>& chunks)
{
    // v1: synchronous warm-up. A background pool comes later if the renderer needs it.
    for (auto& c : chunks) chunk(level, c[0], c[1], c[2]);
}

}  // namespace vc
