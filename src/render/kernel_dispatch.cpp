#include "render/sample_kernel.hpp"
namespace vc {
void compositeRunAVX2(const RowKernelArgs&, int, const Vec3f*, const Vec3f*, std::uint8_t*, bool&);
void compositeRunAVX512(const RowKernelArgs&, int, const Vec3f*, const Vec3f*, std::uint8_t*, bool&);
void compositeRunScalar(const RowKernelArgs& a, int n, const Vec3f* base, const Vec3f* nrm,
                        std::uint8_t* gray, bool& missed) { compositeRun<1>(a,n,base,nrm,gray,missed); }

CompositeRunFn pickCompositeRun() {
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"))
        return &compositeRunAVX512;
    if (__builtin_cpu_supports("avx2"))
        return &compositeRunAVX2;
    return &compositeRunScalar;
}
}
