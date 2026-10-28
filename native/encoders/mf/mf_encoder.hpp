// Media Foundation backend, driven by select.hpp (create_windows.cpp).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../select.hpp"

namespace qb::enc {

// Hardware then software H.264 MFTs taking NV12. gpu_device (ID3D11Device*, may be null) marks
// which hardware MFTs sit on its adapter's vendor.
std::vector<Candidate> mf_candidates(void* gpu_device);

// The attempt's MFT with its vendor settings; Strips = one MFT per strip.
std::unique_ptr<Encoder> create_mf(const Attempt& a, std::string* error);

}  // namespace qb::enc
