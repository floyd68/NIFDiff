#include "ImagePane.h"

#include "../core/NifLog.h" // NIFLOG_* (shared app logger)

#include <Backplate.h>
#include <Image.h>

#include "ImageCore/ImageLoader.h"
#include "ImageCore/ImageRequest.h"
#include "ImageCore/DecodedImage.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace nsk
{

// An FD2D::Image that accepts a decoded CPU BGRA8 payload from a worker thread
// and uploads it to a device bitmap on the UI thread at render time (a bitmap
// can only be created once a render target is in hand). The payload is retained
// so it can be re-uploaded after a device/target loss.
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
        Clear(); // FD2D::Image::Clear (drops the current bitmap)
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
        // The device bitmap is gone; re-upload the retained payload next render.
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_payload.blocks && !m_payload.blocks->empty())
            m_needUpload = true;
    }

    void OnRender(ID2D1RenderTarget* target) override
    {
        if (target)
        {
            ImageCore::DecodedImage snapshot;
            bool upload = false;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (m_needUpload && m_payload.blocks && !m_payload.blocks->empty())
                {
                    snapshot = m_payload; // shared_ptr blob: cheap copy
                    upload = true;
                    m_needUpload = false;
                }
            }
            if (upload)
            {
                const uint32_t pitch = snapshot.rowPitchBytes ? snapshot.rowPitchBytes
                                                              : snapshot.width * 4;
                const D2D1_SIZE_U size { snapshot.width, snapshot.height };
                const auto props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
                if (SUCCEEDED(target->CreateBitmap(size, snapshot.blocks->data(), pitch, props, &bmp)))
                    SetBitmap(bmp);
            }
        }
        FD2D::Image::OnRender(target);
    }

private:
    std::mutex m_mutex;
    ImageCore::DecodedImage m_payload;            // retained for re-upload
    bool m_needUpload = false;
    std::shared_ptr<FD2D::AsyncRedrawToken> m_redraw;
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

    // Supersede any previous load: cancel it and bump the generation so a late
    // completion for the old path is dropped.
    if (m_handle)
    {
        ImageCore::ImageLoader::Instance().Cancel(m_handle);
        m_handle = 0;
    }
    const uint64_t myGen = m_guard->gen.fetch_add(1, std::memory_order_relaxed) + 1;

    ImageCore::ImageRequest request(path, ImageCore::ImagePurpose::FullResolution);
    request.allowGpuCompressedDDS = false; // CPU BGRA8 for the D2D bitmap path
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

void ImagePane::UpdatePathLabel()
{
    m_pathLabel->SetText(m_path.empty() ? L"(no image)" : m_path);
    m_pathLabel->SetCopyText(m_path);
    m_pathLabel->Invalidate();
}

} // namespace nsk
