#include "qk/cuda_candidate_generator.h"

using namespace std;

namespace qk {

bool cuda_candidate_backend_available() {
    return false;
}

string cuda_candidate_backend_unavailable_reason() {
    return "CUDA candidate backend was not built; rebuild with QK_ENABLE_CUDA=ON.";
}

CudaLayerGenerationResult generate_layer_only_candidates_cuda(
    const vector<CudaLayerParent>&,
    const vector<Edge>&,
    int,
    int,
    int,
    long long,
    const vector<Fingerprint128>&,
    size_t,
    uint64_t,
    const vector<Fingerprint128>&,
    bool,
    bool,
    bool
) {
    return {
        false,
        false,
        0,
        {},
        cuda_candidate_backend_unavailable_reason(),
        {}
    };
}

} // namespace qk
