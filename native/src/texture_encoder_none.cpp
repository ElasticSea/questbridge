// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// No texture encoder on this platform yet (macOS: Metal blit into an IOSurface-backed
// CVPixelBuffer + VideoToolbox, roadmap M3/M6).
#include "texture_encoder.hpp"

namespace qb {

std::unique_ptr<TextureEncoder> create_texture_encoder(const TextureEncoderConfig&, std::string* error) {
    if (error) *error = "texture encoding not implemented on this platform yet";
    return nullptr;
}

}  // namespace qb
