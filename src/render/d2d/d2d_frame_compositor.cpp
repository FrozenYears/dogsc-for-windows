#include "dogsc/d2d_frame_compositor.hpp"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <numbers>
#include <stdexcept>
#include <utility>

#include "dogsc/project_model.hpp"

namespace dogsc {
namespace {
using Microsoft::WRL::ComPtr;
using Image = ComPtr<ID2D1Image>;
constexpr double pi = std::numbers::pi_v<double>;

void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(operation) + ": " + code);
    }
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
float f(double value) { return static_cast<float>(value); }
float linear(double value) {
    return f(value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4));
}
D2D1_COLOR_F rgb(double r, double g, double b, double a = 1) {
    return {linear(r), linear(g), linear(b), f(a)};
}
D2D1_COLOR_F color(const HexColor& value, double opacity = 1) {
    require(value.value.size() == 7 || value.value.size() == 9, "Invalid #RRGGBB/#RRGGBBAA color");
    require(value.value.front() == '#', "Color must start with #");
    unsigned channels[4]{0, 0, 0, 255};
    for (size_t i = 1; i < value.value.size(); ++i) {
        const char c = value.value[i];
        const int n = c >= '0' && c <= '9' ? c - '0' :
                      c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                      c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        require(n >= 0, "Invalid hex color digit");
        const size_t channel = (i - 1) / 2;
        channels[channel] = ((i - 1) % 2 == 0) ? static_cast<unsigned>(n * 16) : channels[channel] + n;
    }
    return rgb(channels[0] / 255.0, channels[1] / 255.0, channels[2] / 255.0,
               channels[3] / 255.0 * opacity);
}
D2D1_RECT_F rect(CompositionRect r) { return {f(r.x), f(r.y), f(r.x + r.width), f(r.y + r.height)}; }
CompositionRect inset(CompositionRect r, double amount) {
    return {r.x + amount, r.y + amount, r.width - amount * 2, r.height - amount * 2};
}
bool valid(CompositionRect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && r.width > 0 && r.height > 0;
}
CompositionRect intersect(CompositionRect a, CompositionRect b) {
    const double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.width, b.x + b.width) - x),
                  std::max(0.0, std::min(a.y + a.height, b.y + b.height) - y)};
}
std::wstring utf16(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
    require(n > 0, "Chrome text is not valid UTF-8");
    std::wstring result(static_cast<size_t>(n), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), result.data(), n) == n,
            "Could not convert chrome text");
    return result;
}

// 直接求解矩形到四边形的单应矩阵，保留透视分母，不以仿射近似三维倾斜。
D2D1_MATRIX_4X4_F homography(CompositionRect source, const ProjectedScreenQuad& quad) {
    require(valid(source), "Invalid screen projection rectangle");
    const std::array<CompositionPoint, 4> from{{{source.x, source.y},
        {source.x + source.width, source.y}, {source.x + source.width, source.y + source.height},
        {source.x, source.y + source.height}}};
    const auto to = quad.corners();
    double a[8][9]{};
    for (size_t i = 0; i < 4; ++i) {
        const auto [x, y] = from[i]; const auto [u, v] = to[i];
        require(std::isfinite(u) && std::isfinite(v), "Non-finite projected screen corner");
        a[i * 2][0] = x; a[i * 2][1] = y; a[i * 2][2] = 1;
        a[i * 2][6] = -u * x; a[i * 2][7] = -u * y; a[i * 2][8] = u;
        a[i * 2 + 1][3] = x; a[i * 2 + 1][4] = y; a[i * 2 + 1][5] = 1;
        a[i * 2 + 1][6] = -v * x; a[i * 2 + 1][7] = -v * y; a[i * 2 + 1][8] = v;
    }
    for (size_t c = 0; c < 8; ++c) {
        size_t pivot = c;
        for (size_t r = c + 1; r < 8; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        require(std::abs(a[pivot][c]) > 1e-12, "Degenerate projected screen quadrilateral");
        for (size_t k = c; k < 9; ++k) std::swap(a[c][k], a[pivot][k]);
        const double divisor = a[c][c];
        for (size_t k = c; k < 9; ++k) a[c][k] /= divisor;
        for (size_t r = 0; r < 8; ++r) if (r != c) {
            const double multiple = a[r][c];
            for (size_t k = c; k < 9; ++k) a[r][k] -= multiple * a[c][k];
        }
    }
    const double w0 = a[6][8] * source.x + a[7][8] * source.y + 1;
    for (const auto p : from)
        require((a[6][8] * p.x + a[7][8] * p.y + 1) * w0 > 1e-10,
                "Screen projection crosses the perspective horizon");
    return {f(a[0][8]), f(a[3][8]), 0, f(a[6][8]),
            f(a[1][8]), f(a[4][8]), 0, f(a[7][8]),
            0, 0, 1, 0, f(a[2][8]), f(a[5][8]), 0, 1};
}
bool identity(const FrameScreenScene& screen) {
    const auto r = screen.projection_rect();
    const std::array<CompositionPoint, 4> expected{{{r.x,r.y},{r.x+r.width,r.y},
        {r.x+r.width,r.y+r.height},{r.x,r.y+r.height}}};
    const auto actual = screen.projected_quad.corners();
    for (size_t i = 0; i < 4; ++i)
        if (std::abs(expected[i].x - actual[i].x) > 0.35 || std::abs(expected[i].y - actual[i].y) > 0.35)
            return false;
    return true;
}
}  // namespace

struct D2DFrameCompositor::Impl {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1ColorContext> srgb, scrgb;
    struct EffectSlot { CLSID clsid; ComPtr<ID2D1Effect> value; };
    std::vector<EffectSlot> effects;
    size_t effect_index = 0;
    struct Output {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID2D1Bitmap1> bitmap;
        UINT width = 0, height = 0;
    } published, pending;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> complete;
    struct MaskEntry { double width=0,height=0,radius=0; Image image; };
    std::vector<MaskEntry> mask_cache;
    size_t next_mask=0;
    CompositionRect canvas{};
    std::string error;
    bool drawing = false;

    void initialize(ID3D11Device* input) {
        require(input != nullptr, "D3D11 device is null");
        require((input->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT) != 0,
                "D3D11 device requires D3D11_CREATE_DEVICE_BGRA_SUPPORT");
        d3d = input;
        d3d->GetImmediateContext(&immediate);
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory)), "Create D2D factory");
        ComPtr<IDXGIDevice> dxgi;
        check(d3d.As(&dxgi), "Query DXGI device");
        check(factory->CreateDevice(dxgi.Get(), &device), "Create D2D device");
        check(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc), "Create D2D context");
        require(dc->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_32BPC_FLOAT),
                "D2D device does not support the required linear float working precision");
        const D3D11_QUERY_DESC completion_desc{D3D11_QUERY_EVENT,0};
        check(d3d->CreateQuery(&completion_desc,&complete),"Create frame completion fence");
        check(dc->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srgb), "Create sRGB context");
        check(dc->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &scrgb), "Create linear sRGB context");
        check(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0), &brush), "Create solid brush");
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())), "Create DirectWrite factory");
        dc->SetDpi(96, 96);
        dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        D2D1_RENDERING_CONTROLS controls{D2D1_BUFFER_PRECISION_32BPC_FLOAT, {0, 0}};
        dc->SetRenderingControls(controls);
    }
    void release_inputs() {
        dc->SetTarget(nullptr);
        for (size_t i = 0; i < effect_index; ++i) {
            auto* effect = effects[i].value.Get();
            if(!effect) continue;
            for (UINT j = 0; j < effect->GetInputCount(); ++j) effect->SetInput(j, nullptr);
        }
        effect_index = 0;
    }
    ComPtr<ID2D1Effect> effect(REFCLSID clsid) {
        if (effect_index == effects.size()) effects.push_back({clsid, {}});
        auto& slot = effects[effect_index++];
        if (!slot.value || !IsEqualCLSID(slot.clsid, clsid)) {
            slot.value.Reset(); slot.clsid = clsid;
            check(dc->CreateEffect(clsid, &slot.value), "Create D2D effect");
        }
        check(slot.value->SetValue(D2D1_PROPERTY_PRECISION,D2D1_BUFFER_PRECISION_32BPC_FLOAT), "Set effect float precision");
        return slot.value;
    }
    Image result(ID2D1Effect* value) {
        Image image; value->GetOutput(&image); return image;
    }
    template<class Fn> Image record(Fn&& draw) {
        require(!drawing, "Nested D2D drawing is not allowed");
        ComPtr<ID2D1CommandList> commands;
        check(dc->CreateCommandList(&commands), "Create command list");
        dc->SetTarget(commands.Get());
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->BeginDraw(); drawing = true;
        try { draw(); } catch (...) {
            dc->EndDraw(); drawing = false; dc->SetTarget(nullptr); throw;
        }
        const HRESULT hr = dc->EndDraw(); drawing = false; dc->SetTarget(nullptr);
        check(hr, "Record layer");
        check(commands->Close(), "Close command list");
        Image image; check(commands.As(&image), "Query layer image"); return image;
    }
    Image crop(Image input, CompositionRect bounds) {
        auto e = effect(CLSID_D2D1Crop); e->SetInput(0, input.Get());
        check(e->SetValue(D2D1_CROP_PROP_RECT, rect(bounds)), "Set crop rectangle");
        return result(e.Get());
    }
    Image affine(Image input, D2D1_MATRIX_3X2_F matrix) {
        auto e = effect(CLSID_D2D12DAffineTransform); e->SetInput(0, input.Get());
        check(e->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, matrix), "Set affine transform");
        check(e->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                         D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC), "Set affine interpolation");
        return result(e.Get());
    }
    Image map(Image input, CompositionRect source, CompositionRect target) {
        require(valid(source) && valid(target), "Invalid source/destination texture mapping");
        const double sx = target.width / source.width, sy = target.height / source.height;
        return affine(crop(input, source), D2D1::Matrix3x2F(f(sx), 0, 0, f(sy),
                      f(target.x - source.x * sx), f(target.y - source.y * sy)));
    }
    Image project(Image input, const FrameScreenScene& screen) {
        if (identity(screen)) return input;
        auto e = effect(CLSID_D2D13DTransform); e->SetInput(0, input.Get());
        check(e->SetValue(D2D1_3DTRANSFORM_PROP_TRANSFORM_MATRIX,
                         homography(screen.projection_rect(), screen.projected_quad)), "Set screen homography");
        check(e->SetValue(D2D1_3DTRANSFORM_PROP_INTERPOLATION_MODE,
                         D2D1_3DTRANSFORM_INTERPOLATION_MODE_LINEAR), "Set perspective interpolation");
        return result(e.Get());
    }
    Image composite(Image foreground, Image background, D2D1_COMPOSITE_MODE mode = D2D1_COMPOSITE_MODE_SOURCE_OVER) {
        auto e = effect(CLSID_D2D1Composite);
        e->SetInput(0, background.Get()); e->SetInput(1, foreground.Get());
        check(e->SetValue(D2D1_COMPOSITE_PROP_MODE, mode), "Set compositing mode");
        return result(e.Get());
    }
    Image masked(Image input, Image mask) {
        return composite(input, mask, D2D1_COMPOSITE_MODE_SOURCE_IN);
    }
    Image opacity(Image input, double amount) {
        if (amount >= 0.999) return input;
        auto e = effect(CLSID_D2D1ColorMatrix); e->SetInput(0, input.Get());
        const D2D1_MATRIX_5X4_F matrix{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,f(amount), 0,0,0,0};
        check(e->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix), "Set opacity");
        return result(e.Get());
    }
    Image blur(Image input, double radius, bool clamp) {
        require(std::isfinite(radius) && radius>=0,"Gaussian blur radius must be finite and nonnegative");
        if (radius <= 0.01) return input;
        D2D1_RECT_F original_bounds{};
        if(clamp) {
            check(dc->GetImageLocalBounds(input.Get(),&original_bounds),"Read blur source bounds");
            if(original_bounds.right<=original_bounds.left || original_bounds.bottom<=original_bounds.top) return input;
            const auto edge=effect(CLSID_D2D1Border); edge->SetInput(0,input.Get());
            check(edge->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X,D2D1_BORDER_EDGE_MODE_CLAMP),"Clamp blur x edge");
            check(edge->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y,D2D1_BORDER_EDGE_MODE_CLAMP),"Clamp blur y edge");
            input=result(edge.Get());
        }
        // 大画布的合法阴影可超过 D2D 单次 250px 上限；高斯卷积的方差可加。
        const double pass_count=std::ceil((radius/250)*(radius/250));
        require(pass_count<=1024,"Gaussian blur radius exceeds the texture working range");
        const int count=std::max(1,static_cast<int>(pass_count));
        const float deviation=f(radius/std::sqrt(static_cast<double>(count)));
        for(int i=0;i<count;++i) {
            auto e = effect(CLSID_D2D1GaussianBlur); e->SetInput(0,input.Get());
            check(e->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,deviation), "Set Gaussian deviation");
            check(e->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_SOFT), "Set blur border");
            check(e->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                             D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY), "Set blur quality");
            input=result(e.Get());
        }
        if(clamp) return crop(input,{original_bounds.left,original_bounds.top,
            original_bounds.right-original_bounds.left,original_bounds.bottom-original_bounds.top});
        return input;
    }
    Image colored(Image mask, D2D1_COLOR_F value) {
        auto e = effect(CLSID_D2D1ColorMatrix); e->SetInput(0, mask.Get());
        // 颜色矩阵使用直 alpha；仅复制输入覆盖率，避免把 sRGB 当成线性 RGB。
        const D2D1_MATRIX_5X4_F matrix{0,0,0,0, 0,0,0,0, 0,0,0,0,
            0,0,0,value.a, value.r,value.g,value.b,0};
        check(e->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix), "Set mask color");
        return result(e.Get());
    }
    Image shadow(Image mask, const FrameShadow& spec) {
        return colored(blur(affine(mask, D2D1::Matrix3x2F::Translation(f(spec.offset.x), f(spec.offset.y))),
                            spec.radius, false), color(spec.color, spec.opacity));
    }
    void fill(CompositionRect bounds, D2D1_COLOR_F value) {
        brush->SetColor(value); dc->FillRectangle(rect(bounds), brush.Get());
    }
    void line(CompositionPoint a, CompositionPoint b, D2D1_COLOR_F value, double width) {
        brush->SetColor(value);
        dc->DrawLine({f(a.x),f(a.y)}, {f(b.x),f(b.y)}, brush.Get(), f(width));
    }
    ComPtr<ID2D1Geometry> corner_geometry(CompositionRect bounds, double radius) {
        require(valid(bounds) && std::isfinite(radius), "Invalid rounded mask geometry");
        radius = std::clamp(radius, 0.0, std::min(bounds.width, bounds.height) / 2);
        if (radius >= std::min(bounds.width,bounds.height)/2 && std::abs(bounds.width-bounds.height)<0.5) {
            ComPtr<ID2D1EllipseGeometry> ellipse;
            check(factory->CreateEllipseGeometry(D2D1::Ellipse({f(bounds.mid_x()),f(bounds.mid_y())},
                    f(bounds.width/2),f(bounds.height/2)), &ellipse), "Create ellipse mask");
            ComPtr<ID2D1Geometry> result; check(ellipse.As(&result), "Query ellipse geometry"); return result;
        }
        ComPtr<ID2D1PathGeometry> path; ComPtr<ID2D1GeometrySink> sink;
        check(factory->CreatePathGeometry(&path), "Create continuous corner path");
        check(path->Open(&sink), "Open continuous corner path");
        const double cap = std::min(radius * 1.52866483, std::min(bounds.width,bounds.height)/2);
        const double scale = cap / 1.52866483;
        sink->BeginFigure({f(bounds.x+cap), f(bounds.y)}, D2D1_FIGURE_BEGIN_FILLED);
        // 三段三次曲线在直边接点具有零曲率，避免普通圆弧的曲率突变。
        const auto quarter = [&](double cx, double cy, double ux, double uy, double vx, double vy) {
            const auto p = [&](double u,double v) { return D2D1_POINT_2F{
                f(cx+scale*(ux*u+vx*v)), f(cy+scale*(uy*u+vy*v))}; };
            sink->AddLine(p(1.52866483,0));
            sink->AddBezier(D2D1::BezierSegment(p(1.08849323,0),p(0.86840689,0),p(0.66993427,0.065496)));
            sink->AddBezier(D2D1::BezierSegment(p(0.37282392,0.16905899),p(0.16905899,0.37282392),p(0.065496,0.66993427)));
            sink->AddBezier(D2D1::BezierSegment(p(0,0.86840689),p(0,1.08849323),p(0,1.52866483)));
        };
        quarter(bounds.x+bounds.width,bounds.y,-1,0,0,1);
        quarter(bounds.x+bounds.width,bounds.y+bounds.height,0,-1,-1,0);
        quarter(bounds.x,bounds.y+bounds.height,1,0,0,-1);
        quarter(bounds.x,bounds.y,0,1,1,0);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED); check(sink->Close(), "Close continuous corner path");
        ComPtr<ID2D1Geometry> result; check(path.As(&result), "Query mask geometry"); return result;
    }
    void rounded(CompositionRect bounds, double radius, D2D1_COLOR_F value) {
        auto geometry = corner_geometry(bounds,radius);
        brush->SetColor(value); dc->FillGeometry(geometry.Get(),brush.Get());
    }
    Image mask(CompositionRect bounds, double radius) {
        require(valid(bounds) && std::isfinite(radius),"Invalid mask bounds/radius");
        radius=std::clamp(radius,0.0,std::min(bounds.width,bounds.height)/2);
        Image image;
        for(const auto& cached:mask_cache)
            if(cached.width==bounds.width && cached.height==bounds.height && cached.radius==radius) {
                image=cached.image; break;
            }
        if(!image) {
            const CompositionRect local{0,0,bounds.width,bounds.height};
            image=record([&]{ rounded(local,radius,{1,1,1,1}); });
            MaskEntry entry{bounds.width,bounds.height,radius,image};
            if(mask_cache.size()<96) mask_cache.push_back(std::move(entry));
            else { mask_cache[next_mask]=std::move(entry); next_mask=(next_mask+1)%mask_cache.size(); }
        }
        return bounds.x==0 && bounds.y==0 ? image:
            affine(image,D2D1::Matrix3x2F::Translation(f(bounds.x),f(bounds.y)));
    }
    Image solid(CompositionRect bounds, D2D1_COLOR_F value) {
        return record([&]{ fill(bounds,value); });
    }
    Image ring(CompositionRect bounds, double radius, double width) {
        return composite(mask(bounds,radius),mask(inset(bounds,width),std::max(0.0,radius-width)),
                         D2D1_COMPOSITE_MODE_SOURCE_OUT);
    }
    Image blend(Image foreground, Image background, Image mask_image) {
        const auto outside = composite(background,mask_image,D2D1_COMPOSITE_MODE_SOURCE_OUT);
        const auto inside = masked(foreground,mask_image);
        return composite(inside,outside,D2D1_COMPOSITE_MODE_PLUS);
    }
    Image texture(ID3D11Texture2D* source, const char* name) {
        require(source != nullptr, name);
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        require(desc.Width > 0 && desc.Height > 0 && desc.Width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
                desc.Height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid texture dimensions");
        require(desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && desc.MipLevels == 1,
                "Compositor inputs must be single-slice, single-sample, single-mip textures");
        require(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                "Compositor input format must be BGRA8/RGBA8 sRGB SDR");
        require((desc.BindFlags&D3D11_BIND_SHADER_RESOURCE)!=0,
                "Input textures require D3D11_BIND_SHADER_RESOURCE for D2D sampling");
        require(dc->IsDxgiFormatSupported(desc.Format),"Input texture format is unsupported on this D2D device");
        ComPtr<ID3D11Device> owner; source->GetDevice(&owner);
        ComPtr<IUnknown> owner_id, compositor_id;
        check(owner.As(&owner_id), "Query input device identity"); check(d3d.As(&compositor_id), "Query compositor identity");
        require(owner_id.Get() == compositor_id.Get(), "Texture belongs to a different D3D11 device; transfer explicitly");
        ComPtr<IDXGISurface> surface; check(source->QueryInterface(IID_PPV_ARGS(&surface)), "Query texture surface");
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96,srgb.Get());
        check(dc->CreateBitmapFromDxgiSurface(surface.Get(),&properties,&bitmap), name);
        auto e = effect(CLSID_D2D1ColorManagement); e->SetInput(0,bitmap.Get());
        check(e->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT,srgb.Get()), "Set input sRGB");
        check(e->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,scrgb.Get()), "Linearize input");
        return result(e.Get());
    }
    CompositionRect extent(ID3D11Texture2D* texture) const {
        require(texture != nullptr, "Missing texture");
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        return {0,0,static_cast<double>(desc.Width),static_cast<double>(desc.Height)};
    }
    struct GradientEntry {
        std::vector<D2D1_GRADIENT_STOP> stops;
        ComPtr<ID2D1GradientStopCollection1> collection;
    };
    std::vector<GradientEntry> gradient_cache;
    size_t next_gradient=0;
    ComPtr<ID2D1GradientStopCollection1> gradient_stops(std::span<const D2D1_GRADIENT_STOP> stops) {
        for(const auto& entry:gradient_cache) {
            if(entry.stops.size()!=stops.size()) continue;
            bool same=true;
            for(size_t i=0;i<stops.size();++i) {
                const auto a=entry.stops[i],b=stops[i];
                if(a.position!=b.position || a.color.r!=b.color.r || a.color.g!=b.color.g ||
                   a.color.b!=b.color.b || a.color.a!=b.color.a) { same=false; break; }
            }
            if(same) return entry.collection;
        }
        ComPtr<ID2D1GradientStopCollection1> collection;
        check(dc->CreateGradientStopCollection(stops.data(),static_cast<UINT>(stops.size()),
            D2D1_COLOR_SPACE_SCRGB,D2D1_COLOR_SPACE_SCRGB,D2D1_BUFFER_PRECISION_32BPC_FLOAT,
            D2D1_EXTEND_MODE_CLAMP,D2D1_COLOR_INTERPOLATION_MODE_STRAIGHT,&collection),"Create linear-light gradient stops");
        GradientEntry entry{std::vector<D2D1_GRADIENT_STOP>(stops.begin(),stops.end()),collection};
        if(gradient_cache.size()<32) gradient_cache.push_back(std::move(entry));
        else { gradient_cache[next_gradient]=std::move(entry); next_gradient=(next_gradient+1)%gradient_cache.size(); }
        return collection;
    }
    Image gradient(CompositionRect bounds, CompositionPoint start, CompositionPoint end,
                   std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        const auto collection=gradient_stops({stops.begin(),stops.size()});
        ComPtr<ID2D1LinearGradientBrush> gradient_brush;
        check(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                {f(start.x),f(start.y)},{f(end.x),f(end.y)}),collection.Get(),&gradient_brush),
                "Create linear gradient brush");
        return record([&]{ dc->FillRectangle(rect(bounds),gradient_brush.Get()); });
    }
    Image radial(CompositionRect bounds, CompositionPoint center, double inner, double outer,
                 D2D1_COLOR_F inside, D2D1_COLOR_F outside, bool gaussian = false) {
        require(std::isfinite(outer) && outer > 0, "Invalid radial gradient radius");
        std::array<D2D1_GRADIENT_STOP,65> stops{};
        size_t count=3;
        if (gaussian) {
            count=stops.size();
            // CIGaussianGradient 的半径是高斯尺度；四倍尺度之外贡献不可见。
            outer *= 4;
            for (int i = 0; i <= 64; ++i) {
                const double t = i / 64.0;
                const float weight = f(std::exp(-0.5 * t * t * 16));
                auto value = inside; value.a *= weight;
                stops[static_cast<size_t>(i)]={f(t),value};
            }
        } else {
            stops[0]={0,inside}; stops[1]={f(std::clamp(inner/outer,0.0,0.999999)),inside}; stops[2]={1,outside};
        }
        const auto collection=gradient_stops({stops.data(),count});
        ComPtr<ID2D1RadialGradientBrush> radial_brush;
        check(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
                {f(center.x),f(center.y)},{0,0},f(outer),f(outer)),collection.Get(),&radial_brush),
                "Create radial gradient brush");
        return record([&]{ dc->FillRectangle(rect(bounds),radial_brush.Get()); });
    }
    int pattern_index(const std::string& preset) {
        constexpr std::array<const char*,5> names{"黑曜石网格","工程白网格","暗夜星阵","建筑极简点","立体等角网"};
        for (int i=0;i<5;++i) if (preset == names[static_cast<size_t>(i)]) return i;
        throw std::runtime_error("Unknown background pattern preset: " + preset);
    }
    struct PatternEntry {
        int index=-1;
        double width=0,height=0,tile=0,alpha=0;
        Image image;
    } pattern_cache;
    Image pattern(int index, double scale, double alpha, double time, bool moving) {
        const std::array<double,5> fractions{0.0425,0.040,0.032,0.030,0.050};
        const double requested = std::max(std::min(canvas.width,canvas.height)*fractions[index]*scale,9.0);
        const double tile = std::round(requested*2)/2;
        alpha = std::round(std::clamp(alpha,0.0,1.0)*100)/100;
        if(!moving && pattern_cache.image && pattern_cache.index==index && pattern_cache.width==canvas.width &&
           pattern_cache.height==canvas.height && pattern_cache.tile==tile && pattern_cache.alpha==alpha)
            return pattern_cache.image;
        const double width = std::max(tile*0.008,0.75);
        const std::array<double,5> backgrounds{0.060,0.970,0.065,0.975,0.075};
        const double tx = moving ? std::fmod(time*requested*(index==2 ? 0.07:0.10),requested) : 0;
        // 上游以画布左下平铺；左上坐标同时需要高度余数和反向 y 位移。
        const double ty=std::fmod(canvas.height,tile)-
            (moving ? std::fmod(time*requested*(index==2 ? 0.04:0.065),requested):0);
        auto image=record([&]{
            fill(canvas,rgb(backgrounds[index],backgrounds[index],backgrounds[index]));
            if (alpha <= 0) return;
            dc->PushAxisAlignedClip(rect(canvas),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (double y=ty-tile;y<canvas.height+tile;y+=tile) {
                for (double x=tx-tile;x<canvas.width+tile;x+=tile) {
                    if (index==2 || index==3) {
                        const double radius=std::max(tile*0.026,0.8);
                        brush->SetColor(index==2 ? rgb(0.76,0.68,0.54,0.90*alpha):rgb(0.58,0.58,0.58,0.75*alpha));
                        dc->FillEllipse(D2D1::Ellipse({f(x+tile/2),f(y+tile/2)},f(radius),f(radius)),brush.Get());
                    } else if (index==4) {
                        line({x,y},{x+tile,y+tile},rgb(0.31,0.29,0.26,alpha),width);
                        line({x,y+tile},{x+tile,y},rgb(0.31,0.29,0.26,alpha),width);
                    } else {
                        brush->SetColor(index==0 ? rgb(0.24,0.24,0.24,alpha):rgb(0.79,0.79,0.79,alpha));
                        dc->DrawRectangle(rect({x+width/2,y+width/2,tile-width,tile-width}),brush.Get(),f(width));
                    }
                }
            }
            dc->PopAxisAlignedClip();
        });
        if(!moving) pattern_cache={index,canvas.width,canvas.height,tile,alpha,image};
        return image;
    }
    Image background(const FrameScene& scene, const FrameRenderResources& resources) {
        const auto& bg=scene.background;
        require(std::isfinite(bg.pattern_scale) && bg.pattern_scale>0 &&
                std::isfinite(bg.pattern_opacity), "Invalid background pattern scale/opacity");
        if (bg.source.kind==BackgroundSourceKind::kPattern)
            return pattern(pattern_index(bg.source.pattern_preset),bg.pattern_scale,bg.pattern_opacity,scene.time,false);
        if (bg.source.kind==BackgroundSourceKind::kDynamicFlow) {
            const auto& preset=bg.source.dynamic_preset;
            if (preset=="赛博微流网格")
                return pattern(0,bg.pattern_scale,bg.pattern_opacity,scene.time,true);
            if (preset=="星辉漫步点阵")
                return pattern(2,bg.pattern_scale,bg.pattern_opacity,scene.time,true);
            require(preset=="极光流体星云", "Unknown dynamic background preset");
            Image image=gradient(canvas,{0,canvas.height},{canvas.width,0},
                {{0,rgb(0.025,0.085,0.20)},{1,rgb(0.015,0.028,0.075)}});
            const double alpha=std::clamp(bg.pattern_opacity,0.0,1.0);
            const std::array<std::array<double,3>,3> phases{{{0.19,0.27,0.56},{0.14,0.22,0.42},{0.11,0.17,0.32}}};
            const std::array<D2D1_COLOR_F,3> colors{rgb(0.08,0.42,0.96,0.82*alpha),
                rgb(0.08,0.83,0.96,0.62*alpha),rgb(0.90,0.97,1,0.70*alpha)};
            for(size_t i=0;i<phases.size();++i) {
                const double offset=static_cast<double>(i)*2.18;
                CompositionPoint center{canvas.width/2+std::cos(scene.time*phases[i][0]+offset)*canvas.width*0.36,
                    canvas.height/2-std::sin(scene.time*phases[i][1]+offset*0.73)*canvas.height*0.34};
                image=composite(radial(canvas,center,0,std::min(canvas.width,canvas.height)*phases[i][2]*bg.pattern_scale,
                    colors[i],{0,0,0,0},true),image);
            }
            return image;
        }
        require(bg.source.kind==BackgroundSourceKind::kProjectImage || bg.source.kind==BackgroundSourceKind::kSystemImage ||
                bg.source.kind==BackgroundSourceKind::kProjectVideo || bg.source.kind==BackgroundSourceKind::kSystemVideo,
                "Unknown background source kind");
        // 上游仅媒体背景在资源缺席时使用明确的默认色，不能套用于屏幕等必需输入。
        if (!resources.wallpaper) return solid(canvas,color({"#2D3342"}));
        auto input=texture(resources.wallpaper,"Wallpaper texture unavailable");
        const auto source=extent(resources.wallpaper);
        const double scale=std::max(canvas.width/source.width,canvas.height/source.height);
        const CompositionRect target{(canvas.width-source.width*scale)/2,(canvas.height-source.height*scale)/2,
            source.width*scale,source.height*scale};
        return crop(blur(crop(map(input,source,target),canvas),bg.blur_radius,true),canvas);
    }
    struct Text {
        ComPtr<IDWriteTextLayout> layout;
        double width=0,height=0;
    };
    struct TextEntry {
        std::string value;
        double size=0,width=0;
        bool medium=false;
        Text text;
    };
    std::vector<TextEntry> text_cache;
    size_t next_text=0;
    Text text(const std::string& value,double size,double maximum_width,bool medium) {
        for(const auto& cached:text_cache)
            if(cached.value==value && cached.size==size && cached.width==maximum_width && cached.medium==medium)
                return cached.text;
        auto wide=utf16(value);
        const auto whitespace=[](wchar_t c) {
            WORD type=0; return GetStringTypeW(CT_CTYPE1,&c,1,&type)!=0 && (type&C1_SPACE)!=0;
        };
        const auto first=std::find_if_not(wide.begin(),wide.end(),whitespace);
        const auto last=std::find_if_not(wide.rbegin(),wide.rend(),whitespace).base();
        if(first>=last) return {};
        if(first!=wide.begin() || last!=wide.end()) wide=std::wstring(first,last);
        ComPtr<IDWriteTextFormat> format;
        check(text_factory->CreateTextFormat(L"Segoe UI",nullptr,medium ? DWRITE_FONT_WEIGHT_MEDIUM:DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,f(size),L"",&format),"Create chrome text format");
        check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP),"Disable chrome wrapping");
        Text result;
        check(text_factory->CreateTextLayout(wide.data(),static_cast<UINT32>(wide.size()),format.Get(),
            f(maximum_width),f(size*3),&result.layout),"Create chrome text layout");
        DWRITE_TEXT_METRICS metrics{}; check(result.layout->GetMetrics(&metrics),"Measure chrome text");
        result.width=metrics.widthIncludingTrailingWhitespace; result.height=metrics.height;
        TextEntry entry{value,size,maximum_width,medium,result};
        if(text_cache.size()<96) text_cache.push_back(std::move(entry));
        else { text_cache[next_text]=std::move(entry); next_text=(next_text+1)%text_cache.size(); }
        return result;
    }
    void draw_text(const Text& value,CompositionPoint center,double max_width,CompositionRect clip,D2D1_COLOR_F tint) {
        if(!value.layout) return;
        const double scale=std::min(1.0,max_width/std::max(value.width,1.0));
        D2D1_MATRIX_3X2_F previous{}; dc->GetTransform(&previous);
        dc->PushAxisAlignedClip(rect(clip),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(f(scale),f(scale))*
            D2D1::Matrix3x2F::Translation(f(center.x-value.width*scale/2),f(center.y-value.height*scale/2)));
        brush->SetColor(tint);
        dc->DrawTextLayout({0,0},value.layout.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->SetTransform(previous); dc->PopAxisAlignedClip();
    }
    Image chrome(const FrameScreenChromeScene& c) {
        require(valid(c.outer_rect) && valid(c.content_rect),"Invalid screen chrome geometry");
        const auto outer=mask(c.outer_rect,c.outer_corner_radius);
        if(c.kind>=FrameScreenChromeKind::kDevicePhonePortrait) {
            auto metal=masked(gradient(c.outer_rect,{c.outer_rect.x,c.outer_rect.y},
                {c.outer_rect.x+c.outer_rect.width,c.outer_rect.y+c.outer_rect.height},
                {{0,rgb(0.52,0.54,0.56)},{1,rgb(0.17,0.18,0.20)}}),outer);
            const double rim=std::max(std::min(c.outer_rect.width,c.outer_rect.height)*0.006,0.8);
            auto inner=colored(mask(inset(c.outer_rect,rim),std::max(c.outer_corner_radius-rim,0.0)),rgb(0.055,0.06,0.065));
            auto highlight=composite(outer,mask(inset(c.outer_rect,rim*0.7),std::max(c.outer_corner_radius-rim,0.0)),
                                     D2D1_COMPOSITE_MODE_SOURCE_OUT);
            return composite(colored(highlight,color(c.separator_color,0.42)),composite(inner,metal));
        }
        const auto t=c.toolbar_rect;
        require(valid(t),"Invalid screen chrome toolbar");
        const double diameter=std::min(std::max(t.height*0.23,1.5),t.height*0.32);
        const double margin=std::max(t.height*0.31,diameter), spacing=diameter*1.62;
        const double controls_end=t.x+margin+spacing*2+diameter;
        const auto title=text(c.title,std::max(t.height*0.30,5.0),std::max(t.width*0.46,2.0),true);
        const double fh=std::max(t.height*0.44,2.0), fm=std::max(t.height*0.28,2.0);
        const double available_min=controls_end+fm, available_max=t.x+t.width-fm;
        const auto address=text(c.browser_address,std::max(fh*0.38,5.0),std::max(t.width,2.0),false);
        const double fw=std::min(std::max(address.width+fh*1.7,fh*3),std::max(available_max-available_min,2.0));
        const double fx=std::min(std::max(t.mid_x(),available_min+fw/2),available_max-fw/2);
        const CompositionRect field{fx-fw/2,t.mid_y()-fh/2,fw,fh};
        auto vectors=record([&]{
            rounded(c.outer_rect,c.outer_corner_radius,color(c.surface_color));
            fill(t,color(c.toolbar_color));
            const double separator=std::max(t.height*0.035,0.75);
            fill({t.x,t.y+t.height-separator,t.width,separator},color(c.separator_color));
            const std::array<HexColor,3> colors{{{"#FF5F57"},{"#FEBC2E"},{"#28C840"}}};
            for(size_t i=0;i<colors.size();++i)
                rounded({t.x+margin+static_cast<double>(i)*spacing,t.mid_y()-diameter/2,diameter,diameter},
                    diameter/2,color(colors[i]));
            if(c.kind==FrameScreenChromeKind::kWindow) {
                if(title.layout) draw_text(title,{t.mid_x(),t.mid_y()},std::max(t.width*0.46,2.0),t,color(c.glyph_color));
                else {
                    const double w=std::min(t.width*0.22,t.height*3.2),h=std::max(t.height*0.11,1.0);
                    rounded({t.mid_x()-w/2,t.mid_y()-h/2,w,h},h/2,color(c.glyph_color,0.38));
                }
            } else {
                rounded(field,fh/2,color(c.field_color));
                const double gd=std::max(fh*0.22,1.0);
                rounded({field.x+fh*0.34,field.mid_y()-gd/2,gd,gd},gd/2,color(c.glyph_color,0.62));
                draw_text(address,{field.mid_x(),field.mid_y()},std::max(fw-fh*1.35,2.0),field,color(c.glyph_color));
            }
        });
        return masked(vectors,outer);
    }
    CompositionRect source_crop(CompositionRect source,const NormalizedCrop& value) {
        const auto c=value.clamped();
        return intersect({source.width*c.x,source.height*c.y,source.width*c.width,source.height*c.height},source);
    }
    struct ScreenLayout {
        CompositionRect crop{},placed{};
        double scale=1;
    };
    ScreenLayout screen_layout(CompositionRect source,const FrameScreenScene& screen) {
        const auto crop_rect=source_crop(source,screen.source_crop);
        require(crop_rect.width>1 && crop_rect.height>1,"Screen source crop must contain at least two pixels per axis");
        require(valid(screen.base_rect) && valid(screen.final_rect),"Invalid screen base/final layout");
        const double scale=std::min(screen.base_rect.width/crop_rect.width,screen.base_rect.height/crop_rect.height)*
            screen.final_rect.width/screen.base_rect.width;
        require(std::isfinite(scale) && scale>0,"Invalid screen layout scale");
        return {crop_rect,{screen.final_rect.mid_x()-crop_rect.width*scale/2,
            screen.final_rect.mid_y()-crop_rect.height*scale/2,crop_rect.width*scale,crop_rect.height*scale},scale};
    }
    Image mosaics(Image input,CompositionRect source,const std::vector<FrameMosaicScene>& items) {
        Image current=input;
        for(const auto& item:items) {
            if(item.style!=MosaicEffectStyle::kBlur || item.transition_progress<=0.001) continue;
            const auto n=item.source_rect.clamped();
            const CompositionRect target=intersect({source.width*n.x,source.height*n.y,
                source.width*n.width,source.height*n.height},source);
            if(target.width<=1 || target.height<=1) continue;
            const double radius=(4+item.intensity*44)*item.transition_progress;
            auto affected=blur(current,radius,true);
            current=blend(affected,current,mask(target,std::min(target.width,target.height)*item.corner_radius));
        }
        return current;
    }
    Image native_screen(Image input,const ScreenLayout& layout,const FrameScreenScene& screen) {
        // 先在原始像素坐标内裁切/遮罩，再一次性映射到最终三维四边形。
        // 不能先缩小到画布再放大，否则录屏文字在 zoom/tilt 时永久丢失细节。
        const CompositionRect aperture{
            layout.crop.x+(screen.final_rect.x-layout.placed.x)/layout.scale,
            layout.crop.y+(screen.final_rect.y-layout.placed.y)/layout.scale,
            screen.final_rect.width/layout.scale,screen.final_rect.height/layout.scale};
        const double radius=(screen.decoration ? screen.decoration->content_corner_radius:screen.corner_radius)/layout.scale;
        auto native=masked(crop(input,layout.crop),mask(aperture,radius));
        if(identity(screen)) return map(native,layout.crop,layout.placed);
        const auto position=[&](double x,double y) {
            const CompositionPoint placed{layout.placed.x+(x-layout.crop.x)*layout.scale,
                layout.placed.y+(y-layout.crop.y)*layout.scale};
            const auto point=screen.projected_quad.project(placed,screen.projection_rect());
            require(point.has_value(),"Cannot project native screen pixel"); return *point;
        };
        const auto c=layout.crop;
        const ProjectedScreenQuad quad{position(c.x,c.y),position(c.x+c.width,c.y),
            position(c.x+c.width,c.y+c.height),position(c.x,c.y+c.height)};
        auto e=effect(CLSID_D2D13DTransform); e->SetInput(0,native.Get());
        check(e->SetValue(D2D1_3DTRANSFORM_PROP_TRANSFORM_MATRIX,homography(layout.crop,quad)),"Set native screen projection");
        check(e->SetValue(D2D1_3DTRANSFORM_PROP_INTERPOLATION_MODE,
            D2D1_3DTRANSFORM_INTERPOLATION_MODE_LINEAR),"Set native screen interpolation");
        return result(e.Get());
    }
    Image click_shape(CompositionPoint pointer,double diameter,double width,bool filled,D2D1_COLOR_F tint) {
        const CompositionRect bounds{pointer.x-diameter/2,pointer.y-diameter/2,diameter,diameter};
        if(filled || width>=diameter/2) return colored(mask(bounds,diameter/2),tint);
        return colored(ring(bounds,diameter/2,width),tint);
    }
    Image cursor(Image input,CompositionRect source,const FrameCursorScene& scene) {
        const auto& l=scene.layout;
        const CompositionRect target{l.origin_x,l.origin_y,l.width,l.height};
        require(valid(target),"Invalid cursor bitmap layout");
        const CompositionPoint pointer{l.origin_x+l.hotspot_x,l.origin_y+l.hotspot_y};
        auto visual=map(input,source,target);
        if(std::abs(scene.rotation_radians)>0.0001)
            visual=affine(visual,D2D1::Matrix3x2F::Rotation(f(-scene.rotation_radians*180/pi),
                          {f(pointer.x),f(pointer.y)}));
        if(scene.shadow && scene.shadow->opacity>0)
            visual=composite(visual,shadow(visual,*scene.shadow));
        if(!scene.is_clicking) return visual;
        auto geometry=scene.click_geometry;
        if(!geometry) geometry=CursorRenderGeometry::click_geometry(scene.click_style,
            scene.click_phase ? scene.click_phase->progress:0.25,l.click_effect_height,scene.click_opacity,scene.click_scale);
        if(!geometry) {
            const double diameter=l.click_effect_width*scene.click_scale;
            return diameter>0 ? composite(visual,click_shape(pointer,diameter,l.click_line_width,false,
                color(scene.effective_click_color(),scene.click_opacity))):visual;
        }
        const auto& g=*geometry;
        Image clicks;
        const auto add=[&](Image item) { clicks=clicks ? composite(item,clicks):item; };
        if(scene.click_style==CursorClickEffectStyle::kGlow) {
            if(g.primary_diameter>0) {
                const double r=g.primary_diameter/2;
                const CompositionRect bounds{pointer.x-r,pointer.y-r,r*2,r*2};
                add(radial(bounds,pointer,0,r,color(scene.effective_click_color(),g.primary_opacity),
                           color(scene.effective_click_color(),0)));
            }
        } else {
            if(g.secondary_diameter>0 && g.secondary_opacity>0.001)
                add(click_shape(pointer,g.secondary_diameter,g.secondary_line_width,g.secondary_is_filled,
                               color(scene.effective_click_color(),g.secondary_opacity)));
            if(g.accent_size>0 && g.accent_opacity>0.001 && g.accent_offset>0) {
                const std::array<CompositionPoint,4> points{{{pointer.x,pointer.y-g.accent_offset},
                    {pointer.x,pointer.y+g.accent_offset},{pointer.x-g.accent_offset,pointer.y},
                    {pointer.x+g.accent_offset,pointer.y}}};
                for(const auto p:points) add(click_shape(p,g.accent_size,0,true,
                    color(scene.effective_click_color(),g.accent_opacity)));
            }
            if(g.primary_diameter>0 && g.primary_opacity>0.001)
                add(click_shape(pointer,g.primary_diameter,g.primary_line_width,g.primary_is_filled,
                               color(scene.effective_click_color(),g.primary_opacity)));
        }
        return clicks ? composite(visual,clicks):visual;
    }
    Image screen(Image source,CompositionRect source_bounds,const FrameScreenScene& scene,Image attached_cursor) {
        const auto layout=screen_layout(source_bounds,scene);
        auto content=native_screen(mosaics(source,source_bounds,scene.mosaics),layout,scene);
        const auto projection_rect=scene.projection_rect();
        const double projection_radius=scene.decoration ? scene.decoration->outer_corner_radius:scene.corner_radius;
        Image card=content;
        if(scene.decoration) {
            card=composite(content,project(chrome(*scene.decoration),scene));
            card=masked(card,project(mask(projection_rect,projection_radius),scene));
        }
        if(scene.border_width>0) {
            const auto outer=inset(projection_rect,-scene.border_width);
            auto border=colored(mask(outer,projection_radius+scene.border_width),color(scene.border_color,scene.border_opacity));
            card=composite(card,project(border,scene));
        }
        if(attached_cursor) card=composite(project(attached_cursor,scene),card);
        if(scene.shadow && scene.shadow->opacity>0) {
            auto alpha=identity(scene) ? mask(projection_rect,projection_radius):card;
            card=composite(card,shadow(alpha,*scene.shadow));
        }
        return card;
    }
    Image camera(Image input,CompositionRect source,const FrameCameraScene& scene) {
        require(valid(scene.rect),"Invalid camera rectangle");
        auto fill=scene.content_fill;
        if(!fill) fill=CameraContentFill::layout(source.width,source.height,scene.rect);
        require(fill.has_value() && valid(fill->source) && valid(fill->visible_source),"Invalid camera aspect-fill layout");
        const auto declared=fill->source, visible=fill->visible_source;
        CompositionRect source_rect{source.width*(visible.x-declared.x)/declared.width,
            source.height*(visible.y-declared.y)/declared.height,
            source.width*visible.width/declared.width,source.height*visible.height/declared.height};
        source_rect=intersect(source_rect,source);
        require(valid(source_rect),"Camera visible source aperture is empty");
        auto placed=map(input,source_rect,scene.rect);
        if(scene.is_mirrored) placed=affine(placed,{-1,0,0,1,f(fill->mirror_axis_x()*2),0});
        const auto inner=mask(scene.rect,scene.corner_radius);
        const auto outer=scene.border_width>0 ? mask(inset(scene.rect,-scene.border_width),
            scene.corner_radius+scene.border_width):inner;
        Image layer=masked(placed,inner);
        if(scene.border_width>0) {
            auto border=composite(outer,inner,D2D1_COMPOSITE_MODE_SOURCE_OUT);
            layer=composite(layer,colored(border,color(scene.border_color)));
        }
        if(scene.shadow && scene.shadow->opacity>0) layer=composite(layer,shadow(outer,*scene.shadow));
        return opacity(layer,scene.opacity);
    }
    Image focus(Image base,CompositionRect source,const FrameScreenScene& screen) {
        if(!screen.focus_effect || screen.focus_effect->progress<=0.001) return base;
        const auto& focus=*screen.focus_effect;
        const auto& fx=focus.effect;
        require(fx.is_valid(),"Invalid focus effect values");
        const auto layout=screen_layout(source,screen);
        const CompositionPoint source_point{focus.center.x*source.width,focus.center.y*source.height};
        if(source_point.x<layout.crop.x || source_point.x>layout.crop.x+layout.crop.width ||
           source_point.y<layout.crop.y || source_point.y>layout.crop.y+layout.crop.height) return base;
        const CompositionPoint center{layout.placed.x+(source_point.x-layout.crop.x)*layout.scale,
            layout.placed.y+(source_point.y-layout.crop.y)*layout.scale};
        const double short_edge=std::min(layout.placed.width,layout.placed.height);
        const double radius=short_edge*fx.size/2;
        const auto bounds=identity(screen) ? canvas:screen.projection_rect();
        Image clear;
        if(fx.shape==FocusEffectShape::kLinear) {
            const double feather=std::max(short_edge*fx.softness*0.4,1.0);
            const double angle=fx.angle_degrees.value_or(0)*pi/180;
            const CompositionPoint normal{-std::sin(angle),std::cos(angle)};
            const double distance=radius+feather;
            clear=gradient(bounds,{center.x-normal.x*distance,center.y-normal.y*distance},
                {center.x+normal.x*distance,center.y+normal.y*distance},
                {{0,{1,1,1,0}},{f(feather/(distance*2)),{1,1,1,1}},
                 {f(1-feather/(distance*2)),{1,1,1,1}},{1,{1,1,1,0}}});
        } else {
            clear=radial(bounds,center,radius,radius+std::max(radius*fx.softness*1.5,0.5),{1,1,1,1},{1,1,1,0});
        }
        if(!identity(screen)) clear=project(clear,screen);
        const double blur_radius=fx.blur*28*std::min(canvas.width,canvas.height)/1080;
        Image outside=base;
        if(blur_radius>0.01) {
            const double scale=std::min(1.0,1600/std::max(canvas.width,canvas.height));
            outside=affine(blur(affine(base,D2D1::Matrix3x2F::Scale(f(scale),f(scale))),blur_radius*scale,true),
                D2D1::Matrix3x2F::Scale(f(1/scale),f(1/scale)));
        }
        if(fx.dimming>0) outside=composite(solid(canvas,{0,0,0,f(fx.dimming)}),outside);
        auto focused=blend(base,outside,clear);
        return crop(composite(opacity(focused,focus.progress),base),canvas);
    }
    Image spotlights(Image base,CompositionRect source,const FrameScreenScene& screen) {
        Image clear;
        double dimming=0;
        const auto layout=screen_layout(source,screen);
        const double content_radius=screen.decoration ? screen.decoration->content_corner_radius:screen.corner_radius;
        Image visible;
        for(const auto& item:screen.mosaics) {
            if(item.style!=MosaicEffectStyle::kSpotlight || item.transition_progress<=0.001) continue;
            dimming=std::max(dimming,item.spotlight_dimming*item.transition_progress);
            const auto n=item.source_rect.clamped();
            const CompositionRect authored{source.width*n.x,source.height*n.y,source.width*n.width,source.height*n.height};
            const auto intersection=intersect(authored,layout.crop);
            if(intersection.width<=1 || intersection.height<=1) continue;
            if(!visible) visible=mask(screen.final_rect,content_radius);
            auto selected=map(crop(mask(authored,std::min(authored.width,authored.height)*item.corner_radius),layout.crop),
                layout.crop,layout.placed);
            selected=project(masked(selected,visible),screen);
            clear=clear ? composite(selected,clear):selected;
        }
        if(!clear || dimming<=0.001) return base;
        const auto outside=composite(solid(canvas,{0,0,0,f(dimming)}),base);
        return crop(blend(base,outside,clear),canvas);
    }
    Image sticker(Image input,CompositionRect source,const FrameStickerScene& scene) {
        const double width=std::max(canvas.width*scene.width*scene.scale,2.0);
        const double height=width*source.height/source.width;
        const CompositionPoint center{canvas.width*(scene.position.x+scene.offset.x),
            canvas.height*(scene.position.y+scene.offset.y)};
        const CompositionRect target{center.x-width/2,center.y-height/2,width,height};
        require(valid(target),"Invalid sticker layout");
        const double radius=std::min(std::max(scene.corner_radius*width/1920,0.0),std::min(width,height)/2);
        const auto inner=mask(target,radius);
        Image card=masked(map(input,source,target),inner);
        if(scene.border_width>0) {
            const double border=std::max(scene.border_width*canvas.width/1920,0.5);
            auto border_mask=composite(mask(inset(target,-border),radius+border),inner,D2D1_COMPOSITE_MODE_SOURCE_OUT);
            card=composite(card,colored(crop(border_mask,canvas),color(scene.border_color)));
        }
        if(scene.shadow_opacity>0.001) {
            const double scale=canvas.width/1920;
            FrameShadow spec;
            spec.opacity=scene.shadow_opacity; spec.radius=scene.shadow_radius*scale;
            spec.offset={scene.shadow_offset.x*scale,scene.shadow_offset.y*scale};
            card=composite(card,crop(shadow(inner,spec),canvas));
        }
        if(std::abs(scene.rotation_radians)>0.0001)
            card=affine(card,D2D1::Matrix3x2F::Rotation(f(scene.rotation_radians*180/pi),{f(center.x),f(center.y)}));
        return opacity(card,scene.opacity);
    }
    void ensure_output(Output& output,UINT width,UINT height) {
        if(output.texture && output.width==width && output.height==height) return;
        Output replacement;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        check(d3d->CreateTexture2D(&desc,nullptr,&replacement.texture),"Create offscreen output texture");
        ComPtr<IDXGISurface> surface; check(replacement.texture.As(&surface),"Query output surface");
        const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96,srgb.Get());
        check(dc->CreateBitmapFromDxgiSurface(surface.Get(),&properties,&replacement.bitmap),"Create output target");
        replacement.width=width; replacement.height=height;
        output=std::move(replacement);
    }
    void render(const FrameRenderPlan& plan,const FrameRenderResources& resources) {
        const auto& scene=plan.scene;
        require(is_valid_layer_order(scene.layer_order),"Invalid layer order: expected background/screen/cursor/spotlight/camera/stickers");
        require(std::isfinite(scene.time),"Scene time is not finite");
        require(scene.color==FrameColorContract::sdr_desktop(),"D2D compositor supports linear sRGB working / sRGB SDR output only");
        require(std::isfinite(scene.canvas_size.width) && std::isfinite(scene.canvas_size.height) &&
            scene.canvas_size.width>=2 && scene.canvas_size.height>=2 &&
            std::floor(scene.canvas_size.width)==scene.canvas_size.width &&
            std::floor(scene.canvas_size.height)==scene.canvas_size.height &&
            scene.canvas_size.width<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
            scene.canvas_size.height<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,"Canvas must have integral dimensions in [2,16384]");
        canvas={0,0,scene.canvas_size.width,scene.canvas_size.height};
        const auto has=[&](FrameLayerRole role) {
            return std::find(scene.layer_order.begin(),scene.layer_order.end(),role)!=scene.layer_order.end();
        };
        const double screen_visibility=(1-std::clamp(scene.sticker_backdrop.screen_suppression,0.0,1.0))*scene.screen.opacity;
        const double camera_visibility=1-std::clamp(scene.sticker_backdrop.camera_suppression,0.0,1.0);
        Image screen_input,cursor_input,cursor_layer,camera_input;
        CompositionRect screen_source{},camera_source{};
        if(screen_visibility>0.001 && (has(FrameLayerRole::kScreen)||has(FrameLayerRole::kSpotlight))) {
            screen_input=texture(resources.screen,"Screen layer requires a screen texture");
            screen_source=extent(resources.screen);
        }
        if(screen_visibility>0.001 && has(FrameLayerRole::kCursor) && scene.cursor && resources.cursor) {
            cursor_input=texture(resources.cursor,"Invalid cursor texture");
            cursor_layer=cursor(cursor_input,extent(resources.cursor),*scene.cursor);
        }
        if(camera_visibility>0.001 && has(FrameLayerRole::kCamera) && scene.camera &&
           scene.camera->opacity>0 && resources.camera) {
            camera_input=texture(resources.camera,"Invalid camera texture"); camera_source=extent(resources.camera);
        }
        const bool attached=cursor_layer && !identity(scene.screen);
        Image image=solid(canvas,{0,0,0,0});
        Image before_camera,visible_camera;
        for(const auto role:scene.layer_order) {
            switch(role) {
            case FrameLayerRole::kBackground:
                image=composite(background(scene,resources),image);
                break;
            case FrameLayerRole::kScreen:
                if(screen_visibility<=0.001) break;
                image=crop(composite(opacity(screen(screen_input,screen_source,scene.screen,
                    attached ? cursor_layer:Image{}),screen_visibility),image),canvas);
                break;
            case FrameLayerRole::kCursor:
                if(!attached && cursor_layer && screen_visibility>0.001)
                    image=crop(composite(opacity(cursor_layer,screen_visibility),image),canvas);
                break;
            case FrameLayerRole::kSpotlight:
                if(screen_visibility>0.001) image=spotlights(focus(image,screen_source,scene.screen),screen_source,scene.screen);
                break;
            case FrameLayerRole::kCamera:
                before_camera=image;
                if(camera_input) {
                    // 透明画布属于摄像头层的 extent；背景模糊必须向周围透明像素扩散。
                    visible_camera=crop(composite(opacity(camera(camera_input,camera_source,*scene.camera),camera_visibility),
                        solid(canvas,{0,0,0,0})),canvas);
                    image=crop(composite(visible_camera,image),canvas);
                }
                break;
            case FrameLayerRole::kStickers:
                if(scene.stickers.empty()) break;
                {
                    const double base_blur=scene.sticker_backdrop.screen_blur*
                        (1-std::clamp(scene.sticker_backdrop.screen_suppression,0.0,1.0));
                    image=crop(blur(before_camera ? before_camera:image,base_blur,true),canvas);
                    if(visible_camera)
                        image=crop(composite(crop(blur(visible_camera,scene.sticker_backdrop.camera_blur,true),canvas),image),canvas);
                    // 顺序已经由 evaluator 解析；不按 layer_index 重排相同时间的贴纸。
                    for(const auto& item:scene.stickers) {
                        if(item.opacity<=0.001) continue;
                        const auto it=resources.stickers.find(item.relative_path);
                        // 与上游一致：未加载的可选叠加媒体不产生任何替代像素。
                        if(it==resources.stickers.end() || !it->second) continue;
                        image=crop(composite(sticker(texture(it->second,"Invalid sticker texture"),extent(it->second),item),image),canvas);
                    }
                }
                break;
            }
        }
        auto encode=effect(CLSID_D2D1ColorManagement); encode->SetInput(0,image.Get());
        check(encode->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT,scrgb.Get()),"Set linear output source");
        check(encode->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,srgb.Get()),"Encode sRGB output");
        ensure_output(pending,static_cast<UINT>(canvas.width),static_cast<UINT>(canvas.height));
        dc->SetTarget(pending.bitmap.Get()); dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->BeginDraw(); drawing=true;
        dc->Clear(D2D1::ColorF(0,0));
        dc->DrawImage(encode.Get(),{0,0},rect(canvas),D2D1_INTERPOLATION_MODE_LINEAR,D2D1_COMPOSITE_MODE_SOURCE_OVER);
        const HRESULT hr=dc->EndDraw(); drawing=false; dc->SetTarget(nullptr);
        check(hr,"Render offscreen frame");
        // EndDraw 提交后等待 GPU 完成，调用方可在返回后立即复用或释放借用输入。
        immediate->End(complete.Get()); immediate->Flush();
        BOOL ready=FALSE;
        for(;;) {
            const HRESULT status=immediate->GetData(complete.Get(),&ready,sizeof(ready),0);
            check(status,"Wait for frame completion");
            if(status==S_OK && ready) break;
            const HRESULT removed=d3d->GetDeviceRemovedReason(); check(removed,"D3D11 device removed");
            SwitchToThread();
        }
        std::swap(published,pending);
    }
};

D2DFrameCompositor::D2DFrameCompositor(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
D2DFrameCompositor::~D2DFrameCompositor()=default;

std::unique_ptr<D2DFrameCompositor> D2DFrameCompositor::create(ID3D11Device* device,std::string& error) {
    try {
        auto impl=std::make_unique<Impl>(); impl->initialize(device);
        error.clear();
        return std::unique_ptr<D2DFrameCompositor>(new D2DFrameCompositor(std::move(impl)));
    } catch(const std::exception& failure) { error=failure.what(); return nullptr; }
}
std::unique_ptr<D2DFrameCompositor> D2DFrameCompositor::create_warp(std::string& error) {
    ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,static_cast<UINT>(std::size(levels)),D3D11_SDK_VERSION,&device,nullptr,nullptr);
    if(FAILED(hr)) {
        try { check(hr,"Create WARP device"); } catch(const std::exception& failure) { error=failure.what(); }
        return nullptr;
    }
    return create(device.Get(),error);
}
bool D2DFrameCompositor::compose(const FrameRenderPlan& plan,const FrameRenderResources& resources) {
    impl_->error.clear();
    try {
        impl_->render(plan,resources);
        impl_->release_inputs();
        return true;
    } catch(const std::exception& failure) {
        if(impl_->drawing) { impl_->dc->EndDraw(); impl_->drawing=false; }
        impl_->release_inputs(); impl_->error=failure.what(); return false;
    }
}
std::string_view D2DFrameCompositor::last_error() const { return impl_->error; }
CompositionSize D2DFrameCompositor::output_size() const {
    return {static_cast<double>(impl_->published.width),static_cast<double>(impl_->published.height)};
}
ID3D11Device* D2DFrameCompositor::device() const { return impl_->d3d.Get(); }
ID3D11Texture2D* D2DFrameCompositor::output_texture() const { return impl_->published.texture.Get(); }
bool D2DFrameCompositor::read_back(std::vector<uint8_t>& out) {
    impl_->error.clear();
    try {
        const auto& output=impl_->published;
        require(output.texture!=nullptr,"No successful compose result to read back");
        D3D11_TEXTURE2D_DESC desc{}; output.texture->GetDesc(&desc);
        if(impl_->staging) {
            D3D11_TEXTURE2D_DESC existing{}; impl_->staging->GetDesc(&existing);
            if(existing.Width!=desc.Width || existing.Height!=desc.Height) impl_->staging.Reset();
        }
        if(!impl_->staging) {
            desc.BindFlags=0; desc.MiscFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            check(impl_->d3d->CreateTexture2D(&desc,nullptr,&impl_->staging),"Create CPU readback texture");
        }
        impl_->immediate->CopyResource(impl_->staging.Get(),output.texture.Get());
        const size_t stride=static_cast<size_t>(output.width)*4;
        out.resize(stride*output.height);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(impl_->immediate->Map(impl_->staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map BGRA output");
        for(UINT row=0;row<output.height;++row)
            std::memcpy(out.data()+stride*row,static_cast<const uint8_t*>(mapped.pData)+
                static_cast<size_t>(mapped.RowPitch)*row,stride);
        impl_->immediate->Unmap(impl_->staging.Get(),0);
        return true;
    } catch(const std::exception& failure) { impl_->error=failure.what(); return false; }
}

}  // namespace dogsc
