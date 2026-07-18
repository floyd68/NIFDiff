#include "ImagePane.h"

#include "ImageGpuResourceCache.h" // path -> SRV LRU (fast re-select)
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
// once a render target / context is in hand). Two payload shapes, both uploaded
// to a D3D texture + SRV (SetShaderResource) when D3D is available - so channel
// isolation (a shader effect) works for every format:
//   - a compressed DDS (BCn blocks): uploaded as-is - fast, no CPU decompress.
//   - CPU BGRA8 pixels (PNG/JPG/TGA, or a non-BCn DDS): uploaded as B8G8R8A8.
// Only the D2D-only fallback renderer (no D3D pass) shows a BGRA8 payload as a
// D2D bitmap instead (SetBitmap), where channel isolation does not apply.
// The payload is retained so it can be re-uploaded after a device/target loss.
class ImagePane::ImageView : public FD2D::Image
{
public:
    explicit ImageView(const std::wstring& name) : FD2D::Image(name) {}

    // Worker thread: stash the payload (with the path it came from, so the GPU
    // upload can pool its SRV by path) and wake the UI to upload it.
    void StagePayload(ImageCore::DecodedImage img, std::wstring path)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_payload = std::move(img);
            m_payloadPath = std::move(path);
            m_needUpload = m_payload.blocks && !m_payload.blocks->empty();
        }
        if (m_redraw)
            m_redraw->RequestAsyncRedraw();
    }

    // UI thread: if `path`'s uploaded SRV is still cached for this device, show
    // it synchronously and skip the decode entirely. Returns false on a miss (or
    // before the view is attached to a device), so the caller decodes normally.
    bool TryApplyCachedSrv(const std::wstring& path)
    {
        FD2D::Backplate* bp = BackplateRef();
        if (!bp)
            return false; // no device yet - can't trust the cache's generation
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        UINT w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        if (!ImageGpuResourceCache::Instance().TryGet(
                path, srv, w, h, fmt, bp->GetGraphicsGeneration().device))
            return false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_payload = {}; // no decode round-trip; nothing pending to upload
            m_payloadPath.clear();
            m_needUpload = false;
        }
        SetShaderResource(srv);
        // The cached format tells us premultiplied (BGRA8) vs straight (BCn), so
        // channel isolation stays correct on a cache-hit re-select too.
        m_srvPremultiplied = !IsBlockCompressed(fmt);
        m_srvPath = path;
        PushDrawState();
        return true;
    }

    void ClearImage()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_payload = {};
            m_payloadPath.clear();
            m_needUpload = false;
        }
        m_srvPremultiplied = false;
        m_srvPath.clear();
        Clear(); // FD2D::Image::Clear (drops the current bitmap/SRV)
        Invalidate();
    }

    // Called by the owning pane so a device-loss reload (see OnGraphicsInvalidated
    // when nothing is retained to re-upload) can re-decode the current path.
    void SetOnNeedReload(std::function<void()> handler) { m_onNeedReload = std::move(handler); }

    void OnAttached(FD2D::Backplate& backplate) override
    {
        FD2D::Image::OnAttached(backplate);
        m_redraw = backplate.GetAsyncRedrawToken();
    }

    void OnGraphicsInvalidated(FD2D::GraphicsInvalidationReason reason,
                               const FD2D::GraphicsGeneration& generation) override
    {
        FD2D::Image::OnGraphicsInvalidated(reason, generation);
        // The device resource is gone. If we still hold the decoded payload,
        // re-upload it next render. If we were showing a cache-served SRV (no
        // payload retained), that SRV died with the device - ask the pane to
        // re-decode the current path (the cache self-flushed on the new gen).
        bool needReload = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_payload.blocks && !m_payload.blocks->empty())
                m_needUpload = true;
            else if (!m_srvPath.empty())
                needReload = true;
        }
        if (needReload && m_onNeedReload)
            m_onNeedReload();
    }

    // D3D pass (runs first): upload the pending payload to a GPU texture+SRV.
    // A compressed BCn DDS uploads its blocks straight (the fast path - a 4K BCn
    // DDS uploads in ~10 ms vs ~2 s to CPU-decompress); a BGRA8 payload uploads
    // as a B8G8R8A8_UNORM texture. Routing BGRA8 through the SRV too (not the D2D
    // bitmap) is what makes channel isolation (R/G/B/A, a shader effect) work for
    // PNG/JPEG/TGA/non-BCn DDS, not just BCn. The D2D bitmap path (OnRender) is
    // now only the D2D-only fallback renderer, where this pass never runs.
    void OnRenderD3D(ID3D11DeviceContext* context) override
    {
        ImageCore::DecodedImage img;
        std::wstring path;
        if (context && TakePending(img, path, TakeMode::AnyForGpu))
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            bool uploaded = false;
            if (device)
            {
                const bool compressed = IsBlockCompressed(img.dxgiFormat);
                // BGRA8 bytes from ImageCore are B,G,R,A per texel - exactly
                // B8G8R8A8_UNORM, so the shader samples them as RGBA correctly.
                const DXGI_FORMAT fmt = compressed ? img.dxgiFormat
                                                   : DXGI_FORMAT_B8G8R8A8_UNORM;
                D3D11_TEXTURE2D_DESC desc {};
                desc.Width = img.width;
                desc.Height = img.height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = fmt;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data {};
                data.pSysMem = img.blocks->data();
                data.SysMemPitch = img.rowPitchBytes ? img.rowPitchBytes
                                   : (compressed ? 0u : img.width * 4);
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &tex)))
                {
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                    if (SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
                    {
                        SetShaderResource(srv);
                        // BGRA8 is premultiplied, BCn is straight - drives the
                        // isolation shader's unpremultiply (see PushDrawState).
                        m_srvPremultiplied = !compressed;
                        PushDrawState();
                        // Pool the uploaded SRV so re-selecting this texture is a
                        // synchronous cache hit (see TryApplyCachedSrv) - now for
                        // BGRA8 images too, not only BCn.
                        m_srvPath = path;
                        if (!path.empty())
                        {
                            uint64_t gen = 0;
                            if (FD2D::Backplate* bp = BackplateRef())
                                gen = bp->GetGraphicsGeneration().device;
                            ImageGpuResourceCache::Instance().Put(
                                path, srv, img.width, img.height, fmt, gen);
                        }
                        uploaded = true;
                    }
                }
            }
            // GPU texture/SRV creation failed: hand the payload back so the D2D
            // pass can still show a BGRA8 image as a bitmap this frame.
            if (!uploaded)
                RestagePayload();
        }
        FD2D::Image::OnRenderD3D(context);
    }

    // D2D pass: show a CPU BGRA8 payload as a D2D bitmap. Reached only by the
    // D2D-only fallback renderer (OnRenderD3D didn't run) or after a GPU upload
    // failure restaged the payload; channel isolation does not apply here.
    void OnRender(ID2D1RenderTarget* target) override
    {
        ImageCore::DecodedImage img;
        std::wstring path;
        if (target && TakePending(img, path, TakeMode::UncompressedForD2D))
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
        // BGRA8 (CPU path) is premultiplied; BCn is straight (see DecodedImage).
        // Tells the isolation shader to unpremultiply color channels for accurate
        // straight-alpha readout - consistent across formats.
        ds.sourcePremultiplied = m_srvPremultiplied;
        SetDrawState(ds);
        Invalidate();
    }

    // True for the block-compressed DDS formats (BC1..BC7, all TYPELESS/UNORM/
    // SRORM/SNORM/UF16/SF16 variants). These are the DXGI 70..99 range - the ones
    // uploaded straight to a GPU texture; everything else ImageCore hands back as
    // BGRA8 for the D2D bitmap path. Explicit so the classification doesn't rely
    // on "not BGRA8" catching every case.
    static bool IsBlockCompressed(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    // Which render pass is asking for the pending payload:
    //  - AnyForGpu: the D3D pass takes anything (compressed BCn OR BGRA8) to
    //    upload to a texture+SRV, so channel isolation (a shader effect) works
    //    for every format, not just BCn.
    //  - UncompressedForD2D: the D2D pass only takes BGRA8, and only when the
    //    D3D pass didn't already consume it (i.e. the D2D-only fallback renderer,
    //    where OnRenderD3D never runs) - a BCn payload can't become a bitmap.
    enum class TakeMode { AnyForGpu, UncompressedForD2D };

    // Hand the pending payload (and the path it came from) to a render pass,
    // consuming the upload flag. Restaged (see RestagePayload) if the GPU upload
    // then fails, so the D2D pass can still show a BGRA8 payload this frame.
    bool TakePending(ImageCore::DecodedImage& out, std::wstring& outPath, TakeMode mode)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_needUpload || !m_payload.blocks || m_payload.blocks->empty())
            return false;
        if (mode == TakeMode::UncompressedForD2D && IsBlockCompressed(m_payload.dxgiFormat))
            return false;
        out = m_payload; // shared_ptr blob: cheap copy
        outPath = m_payloadPath;
        m_needUpload = false;
        return true;
    }

    // Put a taken payload back up for upload (the GPU texture/SRV creation
    // failed). m_payload is still retained, so this just re-arms the flag.
    void RestagePayload()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_payload.blocks && !m_payload.blocks->empty())
            m_needUpload = true;
    }

    std::mutex m_mutex;
    ImageCore::DecodedImage m_payload;            // retained for re-upload
    std::wstring m_payloadPath;                   // path the payload decoded from
    bool m_needUpload = false;
    std::wstring m_srvPath;                        // path of the SRV on screen now
    std::function<void()> m_onNeedReload;          // device-loss re-decode request
    std::shared_ptr<FD2D::AsyncRedrawToken> m_redraw;

    // View transform (aspect-fit at zoom 1; pan in client pixels).
    static constexpr float kMinZoom = 0.02f;
    static constexpr float kMaxZoom = 64.0f;
    float m_zoom = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;
    int m_rotation = 0;      // 0/1/2/3 quarter-turns CW
    int m_channelMode = 0;   // 0=RGBA, 1=R, 2=G, 3=B, 4=A
    bool m_srvPremultiplied = false; // current SRV holds premultiplied color (BGRA8)
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
    : PaneContent(name)
{
    m_pathLabel = std::make_shared<FD2D::Text>(name + L"_Path");
    m_pathLabel->SetFont(L"Segoe UI", 13.0f);
    m_pathLabel->SetEllipsisTrimmingEnabled(true);
    m_pathLabel->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.78f));
    m_pathLabel->SetTooltipOnTruncation(true);
    m_pathLabel->SetCopyTextOnRightClick(true);

    m_image = std::make_shared<ImageView>(name + L"_Image");

    // Just the CONTENT of a ComparePane (the frame owns the folder strip):
    // path label (top) + the image (Fill last).
    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_image);
    SetChildDock(m_image, FD2D::Dock::Fill);

    m_guard = std::make_shared<LoadGuard>();
    m_guard->view = m_image;

    // On device loss a cache-served SRV (no retained payload) can't be
    // re-uploaded - re-decode the current path instead (the pooled cache has
    // already self-flushed for the new device generation).
    m_image->SetOnNeedReload([this]
    {
        if (!m_path.empty())
            Load(m_path);
    });

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

    // Report the open (MRU / session) - the path is a real texture the user
    // chose; the decode/cache path below just decides how fast it appears.
    NotifyFileOpened(path);

    // Supersede any previous load: cancel it and bump the generation so a late
    // completion for the old path is dropped.
    if (m_handle)
    {
        ImageCore::ImageLoader::Instance().Cancel(m_handle);
        m_handle = 0;
    }
    const uint64_t myGen = m_guard->gen.fetch_add(1, std::memory_order_relaxed) + 1;

    // Fast re-select: if this texture's uploaded SRV is still pooled for the
    // live device, show it now and skip the whole decode round-trip. Both BCn
    // and BGRA8 SRVs are pooled now, so a re-selected PNG/JPG is a cache hit too
    // (and channel isolation applies to it, since it comes back as an SRV).
    if (m_image && m_image->TryApplyCachedSrv(path))
    {
        Invalidate();
        return true;
    }

    ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::FullResolution);
    // Keep BCn DDS compressed: upload the blocks straight to a GPU texture
    // (ImageView's D3D pass) instead of CPU-decompressing to BGRA8 - ~200x
    // faster for a 4K DDS. Non-DDS decodes to BGRA8, uploaded to an SRV by the
    // same D3D pass (so channel isolation applies).
    request.allowGpuCompressedDDS = true;
    request.srgb = true;

    std::shared_ptr<LoadGuard> guard = m_guard;
    m_handle = ImageCore::ImageLoader::Instance().RequestDecoded(
        request, [guard, myGen, path](HRESULT hr, ImageCore::DecodedImage image)
        {
            // Worker thread. Drop if superseded/stale or the pane is gone.
            if (guard->gen.load(std::memory_order_relaxed) != myGen)
                return;
            if (FAILED(hr) || !image.blocks || image.blocks->empty())
                return;
            if (auto view = guard->view.lock())
                view->StagePayload(std::move(image), path);
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
