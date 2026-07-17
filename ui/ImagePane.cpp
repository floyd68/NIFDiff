#include "ImagePane.h"

#include "../core/NifLog.h" // NIFLOG_* (shared app logger)

#include <Backplate.h>
#include <Image.h>
#include <Util.h> // FD2D::Util::RectContainsPoint

#include "ImageCore/ImageLoader.h"
#include "ImageCore/ImageRequest.h"
#include "ImageCore/DecodedImage.h"

#include <d2d1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <utility>

namespace nsk
{

// An FD2D::Image that accepts a decoded image payload from a worker thread and
// uploads it to the device at render time (a device resource can only be built
// once a render target / context is in hand). Two payload shapes:
//   - a compressed DDS (BCn blocks): uploaded as-is to a D3D texture + SRV
//     (SetShaderResource) - fast, no CPU decompress.
//   - CPU BGRA8 pixels (PNG/JPG/TGA, or a non-BCn DDS): a D2D bitmap (SetBitmap).
// The payload is retained so it can be re-uploaded after a device/target loss.
class ImagePane::ImageView : public FD2D::Image
{
public:
    explicit ImageView(const std::wstring& name) : FD2D::Image(name) {}

    // Worker thread: stash the payload and wake the UI to upload it.
    void StagePayload(ImageCore::DecodedImage img)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_payload = std::move(img);
            m_needUpload = m_payload.blocks && !m_payload.blocks->empty();
        }
        if (m_redraw)
            m_redraw->RequestAsyncRedraw();
    }

    void ClearImage()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_payload = {};
            m_needUpload = false;
        }
        Clear(); // FD2D::Image::Clear (drops the current bitmap/SRV)
        Invalidate();
    }

    void OnAttached(FD2D::Backplate& backplate) override
    {
        FD2D::Image::OnAttached(backplate);
        m_redraw = backplate.GetAsyncRedrawToken();
    }

    void OnGraphicsInvalidated(FD2D::GraphicsInvalidationReason reason,
                               const FD2D::GraphicsGeneration& generation) override
    {
        FD2D::Image::OnGraphicsInvalidated(reason, generation);
        // The device resource is gone; re-upload the retained payload next render.
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_payload.blocks && !m_payload.blocks->empty())
            m_needUpload = true;
    }

    // D3D pass (runs first): upload a compressed BCn payload straight to a GPU
    // texture. This is the fast path - a 4K BCn DDS uploads in ~10 ms vs ~2 s to
    // CPU-decompress it to BGRA8.
    void OnRenderD3D(ID3D11DeviceContext* context) override
    {
        ImageCore::DecodedImage img;
        if (context && TakePending(img, /*wantCompressed=*/true))
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            if (device)
            {
                D3D11_TEXTURE2D_DESC desc {};
                desc.Width = img.width;
                desc.Height = img.height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = img.dxgiFormat;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data {};
                data.pSysMem = img.blocks->data();
                data.SysMemPitch = img.rowPitchBytes;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &tex)))
                {
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                    if (SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
                        SetShaderResource(srv);
                }
            }
        }
        FD2D::Image::OnRenderD3D(context);
    }

    // D2D pass: upload a CPU BGRA8 payload as a D2D bitmap.
    void OnRender(ID2D1RenderTarget* target) override
    {
        ImageCore::DecodedImage img;
        if (target && TakePending(img, /*wantCompressed=*/false))
        {
            const uint32_t pitch = img.rowPitchBytes ? img.rowPitchBytes : img.width * 4;
            const D2D1_SIZE_U size { img.width, img.height };
            const auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
            if (SUCCEEDED(target->CreateBitmap(size, img.blocks->data(), pitch, props, &bmp)))
                SetBitmap(bmp);
        }
        FD2D::Image::OnRender(target);
    }

    // Reset to aspect-fit, unrotated. Called when a new image loads; does NOT
    // notify sync listeners (loading one pane shouldn't reset the others).
    void ResetView()
    {
        m_zoom = 1.0f;
        m_panX = 0.0f;
        m_panY = 0.0f;
        m_rotation = 0;
        m_channelMode = 0;
        PushDrawState();
    }

    void RotateCW()  { m_rotation = (m_rotation + 1) & 3; ApplyDrawState(); }
    void RotateCCW() { m_rotation = (m_rotation + 3) & 3; ApplyDrawState(); }
    void ToggleCheckerboard() { m_checkerboard = !m_checkerboard; ApplyDrawState(); }
    // 0=RGBA, 1=R, 2=G, 3=B, 4=A. Toggling the current channel off returns to RGBA.
    void SetChannelMode(int mode) { m_channelMode = (m_channelMode == mode) ? 0 : mode; ApplyDrawState(); }
    int ChannelMode() const { return m_channelMode; }

    // Shareable transform for cross-pane sync.
    ImagePane::ImageViewState GetState() const
    {
        ImagePane::ImageViewState s;
        s.zoom = m_zoom; s.panX = m_panX; s.panY = m_panY;
        s.rotation = m_rotation; s.channelMode = m_channelMode; s.checkerboard = m_checkerboard;
        return s;
    }
    // Apply an externally-driven transform WITHOUT notifying (no sync feedback).
    void SetState(const ImagePane::ImageViewState& s)
    {
        m_zoom = s.zoom; m_panX = s.panX; m_panY = s.panY;
        m_rotation = s.rotation; m_channelMode = s.channelMode; m_checkerboard = s.checkerboard;
        PushDrawState();
    }
    void SetOnViewChanged(std::function<void(const ImagePane::ImageViewState&)> handler)
    {
        m_onViewChanged = std::move(handler);
    }

    // Mouse wheel zooms toward the cursor; left-drag pans; double-click resets.
    bool OnInputEvent(const FD2D::InputEvent& event) override
    {
        switch (event.type)
        {
        case FD2D::InputEventType::MouseWheel:
            if (event.hasPoint && FD2D::Util::RectContainsPoint(LayoutRect(), event.point))
            {
                const float oldZoom = m_zoom;
                const float notches = static_cast<float>(event.wheelDelta) / 120.0f;
                m_zoom = std::clamp(m_zoom * std::pow(1.2f, notches), kMinZoom, kMaxZoom);
                // Keep the texel under the cursor pinned: pan' = pan + rel*(1 - z'/z),
                // rel = cursor - content-center (content is centred + panned).
                const D2D1_RECT_F rc = LayoutRect();
                const float cx = (rc.left + rc.right) * 0.5f;
                const float cy = (rc.top + rc.bottom) * 0.5f;
                const float relX = static_cast<float>(event.point.x) - cx - m_panX;
                const float relY = static_cast<float>(event.point.y) - cy - m_panY;
                const float k = 1.0f - m_zoom / oldZoom;
                m_panX += relX * k;
                m_panY += relY * k;
                ApplyDrawState();
                return true;
            }
            return false;

        case FD2D::InputEventType::MouseDown:
            if (event.button == FD2D::MouseButton::Left && event.hasPoint &&
                FD2D::Util::RectContainsPoint(LayoutRect(), event.point))
            {
                m_panning = true;
                m_dragStartX = event.point.x;
                m_dragStartY = event.point.y;
                m_panStartX = m_panX;
                m_panStartY = m_panY;
                return true;
            }
            return false;

        case FD2D::InputEventType::MouseMove:
            if (m_panning)
            {
                if (!event.modifiers.leftButton) { m_panning = false; return false; }
                m_panX = m_panStartX + static_cast<float>(event.point.x - m_dragStartX);
                m_panY = m_panStartY + static_cast<float>(event.point.y - m_dragStartY);
                ApplyDrawState();
                return true;
            }
            return false;

        case FD2D::InputEventType::MouseUp:
            if (event.button == FD2D::MouseButton::Left && m_panning)
            {
                m_panning = false;
                return true;
            }
            return false;

        case FD2D::InputEventType::MouseDoubleClick:
            if (event.hasPoint && FD2D::Util::RectContainsPoint(LayoutRect(), event.point))
            {
                ResetView();
                if (m_onViewChanged) m_onViewChanged(GetState()); // a user reset syncs
                return true;
            }
            return false;

        default:
            return false;
        }
    }

private:
    // Input-driven change: push to the display AND notify sync listeners.
    void ApplyDrawState()
    {
        PushDrawState();
        if (m_onViewChanged)
            m_onViewChanged(GetState());
    }
    // Push the current transform to FD2D::Image without notifying (so applying
    // a synced transform from another pane doesn't loop back).
    void PushDrawState()
    {
        FD2D::Image::DrawState ds;
        ds.zoomScale = m_zoom;
        ds.panX = m_panX;
        ds.panY = m_panY;
        ds.rotationQuarters = m_rotation;
        ds.highQualitySampling = true;
        ds.alphaCheckerboardEnabled = m_checkerboard;
        ds.channelMode = m_channelMode;
        SetDrawState(ds);
        Invalidate();
    }

    static bool IsBgra8(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    // Hand the pending payload to whichever render pass matches its format
    // (compressed BCn -> D3D pass, BGRA8 -> D2D pass), consuming the upload flag.
    bool TakePending(ImageCore::DecodedImage& out, bool wantCompressed)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_needUpload || !m_payload.blocks || m_payload.blocks->empty())
            return false;
        const bool isCompressed = !IsBgra8(m_payload.dxgiFormat);
        if (isCompressed != wantCompressed)
            return false;
        out = m_payload; // shared_ptr blob: cheap copy
        m_needUpload = false;
        return true;
    }

    std::mutex m_mutex;
    ImageCore::DecodedImage m_payload;            // retained for re-upload
    bool m_needUpload = false;
    std::shared_ptr<FD2D::AsyncRedrawToken> m_redraw;

    // View transform (aspect-fit at zoom 1; pan in client pixels).
    static constexpr float kMinZoom = 0.02f;
    static constexpr float kMaxZoom = 64.0f;
    float m_zoom = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;
    int m_rotation = 0;      // 0/1/2/3 quarter-turns CW
    int m_channelMode = 0;   // 0=RGBA, 1=R, 2=G, 3=B, 4=A
    bool m_checkerboard = false;
    bool m_panning = false;
    LONG m_dragStartX = 0;
    LONG m_dragStartY = 0;
    float m_panStartX = 0.0f;
    float m_panStartY = 0.0f;
    std::function<void(const ImagePane::ImageViewState&)> m_onViewChanged;
};

struct ImagePane::LoadGuard
{
    std::atomic<uint64_t> gen { 0 };
    std::weak_ptr<ImageView> view;
};

ImagePane::ImagePane(const std::wstring& name)
    : ComparePane(name)
{
    m_pathLabel = std::make_shared<FD2D::Text>(name + L"_Path");
    m_pathLabel->SetFont(L"Segoe UI", 13.0f);
    m_pathLabel->SetEllipsisTrimmingEnabled(true);
    m_pathLabel->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.78f));
    m_pathLabel->SetTooltipOnTruncation(true);
    m_pathLabel->SetCopyTextOnRightClick(true);

    m_image = std::make_shared<ImageView>(name + L"_Image");

    // Same dock order as NifComparePane: Top strip first, Fill last.
    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_image);
    SetChildDock(m_image, FD2D::Dock::Fill);

    m_guard = std::make_shared<LoadGuard>();
    m_guard->view = m_image;

    UpdatePathLabel();
}

ImagePane::~ImagePane()
{
    // Invalidate any in-flight decode: bump the generation (its completion
    // becomes a no-op) and cancel the outstanding request.
    if (m_guard)
        m_guard->gen.fetch_add(1, std::memory_order_relaxed);
    if (m_handle)
        ImageCore::ImageLoader::Instance().Cancel(m_handle);
}

bool ImagePane::Load(const std::wstring& path, std::string* /*error*/)
{
    if (path.empty())
        return false;

    m_path = path;
    UpdatePathLabel();
    if (m_image)
        m_image->ResetView(); // each new image starts aspect-fit, unrotated

    // Supersede any previous load: cancel it and bump the generation so a late
    // completion for the old path is dropped.
    if (m_handle)
    {
        ImageCore::ImageLoader::Instance().Cancel(m_handle);
        m_handle = 0;
    }
    const uint64_t myGen = m_guard->gen.fetch_add(1, std::memory_order_relaxed) + 1;

    ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::FullResolution);
    // Keep BCn DDS compressed: upload the blocks straight to a GPU texture
    // (ImageView's D3D pass) instead of CPU-decompressing to BGRA8 - ~200x
    // faster for a 4K DDS. Non-DDS still decodes to BGRA8 (D2D pass).
    request.allowGpuCompressedDDS = true;
    request.srgb = true;

    std::shared_ptr<LoadGuard> guard = m_guard;
    m_handle = ImageCore::ImageLoader::Instance().RequestDecoded(
        request, [guard, myGen](HRESULT hr, ImageCore::DecodedImage image)
        {
            // Worker thread. Drop if superseded/stale or the pane is gone.
            if (guard->gen.load(std::memory_order_relaxed) != myGen)
                return;
            if (FAILED(hr) || !image.blocks || image.blocks->empty())
                return;
            if (auto view = guard->view.lock())
                view->StagePayload(std::move(image));
        });

    Invalidate();
    return true;
}

void ImagePane::Clear()
{
    if (m_guard)
        m_guard->gen.fetch_add(1, std::memory_order_relaxed);
    if (m_handle)
    {
        ImageCore::ImageLoader::Instance().Cancel(m_handle);
        m_handle = 0;
    }
    m_path.clear();
    if (m_image)
        m_image->ClearImage();
    UpdatePathLabel();
    Invalidate();
}

void ImagePane::SetChannelMode(int mode) { if (m_image) m_image->SetChannelMode(mode); }
void ImagePane::ToggleAlphaCheckerboard() { if (m_image) m_image->ToggleCheckerboard(); }
void ImagePane::RotateCW() { if (m_image) m_image->RotateCW(); }
void ImagePane::RotateCCW() { if (m_image) m_image->RotateCCW(); }
void ImagePane::ResetView() { if (m_image) m_image->ResetView(); }

ImagePane::ImageViewState ImagePane::ViewState() const
{
    return m_image ? m_image->GetState() : ImageViewState {};
}
void ImagePane::SetViewState(const ImageViewState& state)
{
    if (m_image) m_image->SetState(state);
}
void ImagePane::SetOnViewChanged(std::function<void(const ImageViewState&)> handler)
{
    if (m_image) m_image->SetOnViewChanged(std::move(handler));
}

void ImagePane::UpdatePathLabel()
{
    m_pathLabel->SetText(m_path.empty() ? L"(no image)" : m_path);
    m_pathLabel->SetCopyText(m_path);
    m_pathLabel->Invalidate();
}

} // namespace nsk
