#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <numeric>
#include <initializer_list>

// Minimal vector + tensor math for volume-commander. No OpenCV.
// One small fixed-size vector type and one contiguous N-D tensor that
// backs everything: 2D coord/normal/framebuffer images, 3D mask volumes,
// chunk data.

namespace vc {

// ---------------------------------------------------------------- Vec<T,N>
template <typename T, int N>
struct Vec {
    std::array<T, N> v{};

    constexpr Vec() = default;
    constexpr Vec(T a, T b) requires (N == 2) : v{a, b} {}
    constexpr Vec(T a, T b, T c) requires (N == 3) : v{a, b, c} {}
    constexpr Vec(T a, T b, T c, T d) requires (N == 4) : v{a, b, c, d} {}
    explicit constexpr Vec(T s) { v.fill(s); }

    constexpr T&       operator[](int i)       noexcept { return v[i]; }
    constexpr const T& operator[](int i) const noexcept { return v[i]; }

    constexpr Vec operator+(const Vec& o) const noexcept { Vec r; for (int i=0;i<N;++i) r[i]=v[i]+o[i]; return r; }
    constexpr Vec operator-(const Vec& o) const noexcept { Vec r; for (int i=0;i<N;++i) r[i]=v[i]-o[i]; return r; }
    constexpr Vec operator*(T s)          const noexcept { Vec r; for (int i=0;i<N;++i) r[i]=v[i]*s;    return r; }
    constexpr Vec operator/(T s)          const noexcept { Vec r; for (int i=0;i<N;++i) r[i]=v[i]/s;    return r; }
    constexpr Vec operator-()             const noexcept { Vec r; for (int i=0;i<N;++i) r[i]=-v[i];     return r; }
    constexpr Vec& operator+=(const Vec& o) noexcept { for (int i=0;i<N;++i) v[i]+=o[i]; return *this; }
    constexpr Vec& operator-=(const Vec& o) noexcept { for (int i=0;i<N;++i) v[i]-=o[i]; return *this; }
    constexpr Vec& operator*=(T s)          noexcept { for (int i=0;i<N;++i) v[i]*=s;    return *this; }

    constexpr bool operator==(const Vec&) const noexcept = default;
};

template <typename T, int N>
constexpr Vec<T,N> operator*(T s, const Vec<T,N>& a) noexcept { return a * s; }

template <typename T, int N>
constexpr T dot(const Vec<T,N>& a, const Vec<T,N>& b) noexcept {
    T s{}; for (int i=0;i<N;++i) s += a[i]*b[i]; return s;
}
template <typename T>
constexpr Vec<T,3> cross(const Vec<T,3>& a, const Vec<T,3>& b) noexcept {
    return { a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0] };
}
template <typename T, int N>
inline T norm(const Vec<T,N>& a) noexcept { return std::sqrt(dot(a,a)); }
template <typename T, int N>
inline Vec<T,N> normalized(const Vec<T,N>& a) noexcept { T n = norm(a); return n > T(0) ? a / n : a; }

using Vec2f = Vec<float, 2>;
using Vec2d = Vec<double, 2>;
using Vec2i = Vec<int, 2>;
using Vec3f = Vec<float, 3>;
using Vec3d = Vec<double, 3>;
using Vec3i = Vec<int, 3>;
using Vec3b = Vec<uint8_t, 3>;
using Vec4f = Vec<float, 4>;

template <typename T>
constexpr T clampv(T x, T lo, T hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }

// ---------------------------------------------------------------- Tensor
// Contiguous row-major N-D tensor. shape[0] is the slowest-varying axis.
// 2D images use shape {rows, cols}; 3D volumes {z, y, x}. Element type T
// can itself be a Vec (e.g. Tensor<Vec3f> for a coordinate field).
template <typename T>
struct Tensor {
    std::vector<int> shape;
    std::vector<std::size_t> stride;  // in elements
    std::vector<T> data;

    Tensor() = default;
    explicit Tensor(std::vector<int> shp) { reshape(std::move(shp)); }
    Tensor(int r, int c) { reshape({r, c}); }
    Tensor(int d, int h, int w) { reshape({d, h, w}); }

    void reshape(std::vector<int> shp) {
        shape = std::move(shp);
        stride.resize(shape.size());
        std::size_t s = 1;
        for (int i = int(shape.size()) - 1; i >= 0; --i) { stride[i] = s; s *= std::size_t(shape[i]); }
        data.assign(s, T{});
    }
    void create(std::vector<int> shp) {
        if (shp != shape) reshape(std::move(shp));
    }
    // Like create() but does NOT zero — for buffers the caller fully overwrites
    // (render framebuffers, coord/normal grids). Skips a big per-frame memset.
    void createUninit(std::vector<int> shp) {
        if (shp == shape) return;
        shape = std::move(shp);
        stride.resize(shape.size());
        std::size_t s = 1;
        for (int i = int(shape.size()) - 1; i >= 0; --i) { stride[i] = s; s *= std::size_t(shape[i]); }
        data.resize(s);   // grows uninitialized for trivial T; reused if same size
    }

    bool empty() const noexcept { return data.empty(); }
    void clear() noexcept { shape.clear(); stride.clear(); data.clear(); }
    int  ndim() const noexcept { return int(shape.size()); }
    std::size_t count() const noexcept { return data.size(); }

    // 2D convenience
    int rows() const noexcept { return shape.size() > 0 ? shape[0] : 0; }
    int cols() const noexcept { return shape.size() > 1 ? shape[1] : 0; }

    T&       operator()(int r, int c)       noexcept { return data[std::size_t(r)*stride[0] + std::size_t(c)*stride[1]]; }
    const T& operator()(int r, int c) const noexcept { return data[std::size_t(r)*stride[0] + std::size_t(c)*stride[1]]; }
    T&       operator()(int z, int y, int x)       noexcept { return data[std::size_t(z)*stride[0] + std::size_t(y)*stride[1] + std::size_t(x)*stride[2]]; }
    const T& operator()(int z, int y, int x) const noexcept { return data[std::size_t(z)*stride[0] + std::size_t(y)*stride[1] + std::size_t(x)*stride[2]]; }

    T*       ptr()       noexcept { return data.data(); }
    const T* ptr() const noexcept { return data.data(); }
    T*       row(int r)       noexcept { return data.data() + std::size_t(r)*stride[0]; }
    const T* row(int r) const noexcept { return data.data() + std::size_t(r)*stride[0]; }

    bool in2(int r, int c) const noexcept { return r>=0 && c>=0 && r<rows() && c<cols(); }
};

using TensorF  = Tensor<float>;
using Tensor3f = Tensor<Vec3f>;
using Tensor3d = Tensor<Vec3d>;
using TensorU8 = Tensor<uint8_t>;
using Tensor32 = Tensor<uint32_t>;  // ARGB framebuffer

}  // namespace vc
