// Windows create_encoder: Media Foundation hardware MFTs by vendor, then OpenH264, then the
// Microsoft software MFT (see select.hpp for the order and the probe check).
#include <algorithm>

#include "mf/d3d11_input.hpp"
#include "mf/mf_encoder.hpp"
#include "openh264/openh264_encoder.hpp"
#include "select.hpp"
#include "strips.hpp"

namespace ql::enc {

std::unique_ptr<Encoder> create_encoder(const EncoderConfig& config, std::string* error) {
    std::vector<Candidate> found = mf_candidates(config.gpu_device);
    std::string missing;
    if (openh264_available(&missing)) {
        Candidate c;
        c.backend = Backend::OpenH264;
        c.name = "OpenH264";
        found.push_back(c);
    }
    auto attempts = plan(found, config);
    auto enc = select(
        attempts,
        [](const Attempt& a, std::string* e) -> std::unique_ptr<Encoder> {
            if (a.candidate.backend == Backend::MediaFoundation) return create_mf(a, e);
            return make_strip_encoder(
                a.config, [](const EncoderConfig& sc, int, std::string* se) { return create_openh264(sc, se); },
                d3d11_read_back_input(), e);
        },
        error);
    if (!enc && error && !missing.empty() && config.allow_software) *error += " [" + missing + "]";
    return enc;
}

}  // namespace ql::enc
