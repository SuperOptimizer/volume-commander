#include "render/sample_kernel.hpp"
namespace vc {
void compositeRunAVX2(const RowKernelArgs& a, int n, const Vec3f* base, const Vec3f* nrm,
                      std::uint8_t* gray, bool& missed) {
    compositeRun<8>(a, n, base, nrm, gray, missed);
}
}
