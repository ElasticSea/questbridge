#include "d3d11_input.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ql::enc {

struct D3D11Input::Impl {
    // split
    std::vector<ComPtr<ID3D11Texture2D>> strips;
    D3D11_TEXTURE2D_DESC strip_desc{};
    // read_back: the staging copy stays mapped until the next call (frames are read in place)
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11DeviceContext> mapped_ctx;
    D3D11_TEXTURE2D_DESC staging_desc{};

    void unmap() {
        if (mapped_ctx) mapped_ctx->Unmap(staging.Get(), 0);
        mapped_ctx.Reset();
    }
};

D3D11Input::D3D11Input() : impl_(std::make_unique<Impl>()) {}
D3D11Input::~D3D11Input() { impl_->unmap(); }

namespace {

bool same(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format;
}

}  // namespace

bool D3D11Input::split(const GpuFrame& in, int n, std::vector<GpuFrame>& out, std::string* error) {
    auto* src = static_cast<ID3D11Texture2D*>(in.texture);
    if (!src || n < 1) {
        if (error) *error = "no texture";
        return false;
    }
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    const UINT h = d.Height / UINT(n);
    ComPtr<ID3D11Device> dev;
    src->GetDevice(&dev);
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.CPUAccessFlags = 0;
    sd.MiscFlags = 0;
    if (impl_->strips.size() != size_t(n) || !same(impl_->strip_desc, sd)) {
        impl_->strips.assign(size_t(n), nullptr);
        for (auto& t : impl_->strips) {
            HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &t);
            if (FAILED(hr)) {
                impl_->strips.clear();
                if (error) *error = "CreateTexture2D(strip) failed";
                return false;
            }
        }
        impl_->strip_desc = sd;
    }
    out.assign(size_t(n), GpuFrame{});
    for (int i = 0; i < n; i++) {
        D3D11_BOX box{0, UINT(i) * h, 0, d.Width, UINT(i + 1) * h, 1};
        ctx->CopySubresourceRegion(impl_->strips[size_t(i)].Get(), 0, 0, 0, 0, src, in.subresource, &box);
        out[size_t(i)].texture = impl_->strips[size_t(i)].Get();
    }
    return true;
}

bool D3D11Input::read_back(const GpuFrame& in, CpuFrame& out, std::string* error) {
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    auto* src = static_cast<ID3D11Texture2D*>(in.texture);
    if (!src) return fail("no texture");
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    const bool nv12 = d.Format == DXGI_FORMAT_NV12;
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!nv12 && !rgba && !bgra) return fail("read back: texture format not NV12/BGRA/RGBA");
    impl_->unmap();
    ComPtr<ID3D11Device> dev;
    src->GetDevice(&dev);
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (!impl_->staging || !same(impl_->staging_desc, sd)) {
        impl_->staging.Reset();
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &impl_->staging))) return fail("CreateTexture2D(staging) failed");
        impl_->staging_desc = sd;
    }
    ctx->CopySubresourceRegion(impl_->staging.Get(), 0, 0, 0, 0, src, in.subresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return fail("Map(staging) failed");
    impl_->mapped_ctx = ctx;
    auto* p = static_cast<const uint8_t*>(m.pData);
    out.plane[0] = p;
    out.stride[0] = int(m.RowPitch);
    if (nv12) {
        out.format = PixelFormat::NV12;
        out.plane[1] = p + size_t(m.RowPitch) * d.Height;
        out.stride[1] = int(m.RowPitch);
    } else {
        out.format = rgba ? PixelFormat::RGBA : PixelFormat::BGRA;
    }
    return true;
}

GpuInput d3d11_split_input(int strips) {
    auto in = std::make_shared<D3D11Input>();
    GpuInput g;
    g.split = [in, strips](const GpuFrame& f, std::vector<GpuFrame>& out, std::string* e) {
        return in->split(f, strips, out, e);
    };
    return g;
}

GpuInput d3d11_read_back_input() {
    auto in = std::make_shared<D3D11Input>();
    GpuInput g;
    g.read_back = [in](const GpuFrame& f, CpuFrame& out, std::string* e) { return in->read_back(f, out, e); };
    return g;
}

}  // namespace ql::enc
