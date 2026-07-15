#include "ThumbnailStrip.h"

#include "../core/NifDocument.h"
#include "../core/SceneBuilder.h"
#include "../core/Camera.h"
#include "../core/ResourceManager.h"
#include "../render/TextureCache.h"

#include <Backplate.h>
#include <Core.h>
#include <Util.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace nsk
{

namespace
{
    constexpr float kPad = 8.0f;
    constexpr float kLabelH = 20.0f;
    constexpr float kGripThickness = 8.0f;  // drag-to-resize band on the inner edge
    constexpr float kMinExtent = 110.0f;    // resize clamp (see SetFixedExtent)
    constexpr float kMaxExtent = 360.0f;
    constexpr float kHeaderH = 26.0f;      // count header (vertical mode only)
    constexpr UINT kThumbPx = 168;         // offscreen render HEIGHT (width follows aspect)
    constexpr int kPerFrame = 2;           // thumbnails generated per frame
    constexpr float kYawRad = 0.1745f;     // ~10 deg yaw (3/4 view) so thumbnails aren't dead-on
    constexpr float kThumbMarginFrac = 0.06f; // equal margin, fraction of the larger extent
    constexpr float kMinAspect = 0.45f;    // clamp card w/h so cards stay reasonable
    constexpr float kMaxAspect = 2.6f;
}

ThumbnailStrip::ThumbnailStrip(const std::wstring& name)
    : FD2D::Wnd(name)
{
}

ThumbnailStrip::~ThumbnailStrip()
{
    // Drop the manager's generation for us so any in-flight parse job's result
    // is discarded instead of delivered to this (now dead) strip.
    if (m_manager)
        m_manager->Cancel(this);
}

void ThumbnailStrip::SetOrientation(Orientation o)
{
    const bool h = (o == Orientation::Horizontal);
    if (h == m_horizontal)
        return;
    m_horizontal = h;
    m_scroll = 0.0f;
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void ThumbnailStrip::SetActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    // Presence (measured extent) changed, so relayout the pane's dock.
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void ThumbnailStrip::SetEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    m_hoverCard = -1;
    if (!enabled)
    {
        // Idle the loader: cancel queued/in-flight parses (bump our generation
        // so the manager drops their results) and drop pending scenes.
        if (m_manager)
            m_gen = m_manager->BumpGeneration(this);
        m_ready.clear();
    }
    else
    {
        // Resume: re-submit any files not rendered before we were turned off.
        EnqueuePending();
    }
    // Toggling presence changes our measured extent (0 when off), so relayout.
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

float ThumbnailStrip::ThumbSide() const
{
    // The thumbnail's FIXED dimension (height when horizontal): the strip
    // thickness minus the label row and padding. Card WIDTH follows the aspect.
    return m_horizontal ? (m_fixedExtent - kLabelH - kPad * 3.0f)
                        : (m_fixedExtent - kPad * 2.0f);
}

float ThumbnailStrip::CardExtent(std::size_t index) const
{
    // Size of one card along the scroll axis. Horizontal cards vary in width
    // with the thumbnail aspect (folder/Up tiles and unrendered files are
    // square, aspect 1); vertical cards keep a uniform height.
    const float thumb = ThumbSide();
    if (m_horizontal)
    {
        const float aspect = (index < m_entries.size()) ? m_entries[index].aspect : 1.0f;
        return thumb * aspect + kPad * 2.0f;
    }
    return thumb + kLabelH + kPad * 2.0f;
}

float ThumbnailStrip::CardOffset(std::size_t index) const
{
    float off = LeadGutter();
    for (std::size_t i = 0; i < index && i < m_entries.size(); ++i)
        off += CardExtent(i);
    return off;
}

float ThumbnailStrip::LeadGutter() const
{
    return m_horizontal ? kPad : kHeaderH;
}

float ThumbnailStrip::ContentExtent() const
{
    float total = LeadGutter() + kPad;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
        total += CardExtent(i);
    return total;
}

void ThumbnailStrip::OnAttached(FD2D::Backplate& backplate)
{
    FD2D::Wnd::OnAttached(backplate);
    m_device = backplate.D3DDevice();
    m_context = backplate.D3DContext();
}

void ThumbnailStrip::ShowForFile(const std::wstring& nifPath)
{
    if (nifPath.empty())
    {
        // The active pane has nothing loaded: clear the strip.
        if (!m_folder.empty() || !m_entries.empty())
            NavigateTo(std::wstring(), std::wstring());
        return;
    }
    std::filesystem::path p(nifPath);
    const std::wstring dir = p.has_parent_path() ? p.parent_path().wstring() : std::wstring();
    if (dir == m_folder && !m_entries.empty())
    {
        // Same folder already listed - just move the highlight, no re-list.
        if (m_currentFile != nifPath)
        {
            m_currentFile = nifPath;
            CenterCurrentFile(); // keep the selection centered in the strip
            Invalidate();
        }
        return;
    }
    NavigateTo(dir, nifPath);
}

void ThumbnailStrip::NavigateTo(std::wstring folder, std::wstring selectPath)
{
    // Cancel in-flight/queued parses from the previous folder: bump our
    // generation (the manager drops older jobs + results) and drop any parsed
    // scenes already queued for render.
    if (m_manager)
        m_gen = m_manager->BumpGeneration(this);
    m_ready.clear();

    m_entries.clear();
    m_scroll = 0.0f;
    m_hoverCard = -1;
    m_folder = folder;
    m_currentFile = selectPath;
    m_thumbCache.Clear();

    auto lessNoCase = [](const Entry& a, const Entry& b)
    {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    };

    if (!folder.empty())
    {
        std::error_code ec;
        std::filesystem::path dir(folder);
        if (std::filesystem::is_directory(dir, ec))
        {
            // ".." to the parent (unless this is a filesystem root).
            const std::filesystem::path parent = dir.parent_path();
            if (!parent.empty() && parent != dir)
            {
                Entry up;
                up.kind = EntryKind::Up;
                up.path = parent.wstring();
                up.name = L"..";
                up.rendered = true;
                m_entries.push_back(std::move(up));
            }

            std::vector<Entry> folders, files;
            for (const auto& de : std::filesystem::directory_iterator(dir, ec))
            {
                if (ec) break;
                std::error_code ec2;
                if (de.is_directory(ec2))
                {
                    Entry f;
                    f.kind = EntryKind::Folder;
                    f.path = de.path().wstring();
                    f.name = de.path().filename().wstring();
                    f.rendered = true;
                    folders.push_back(std::move(f));
                }
                else if (de.is_regular_file(ec2))
                {
                    std::wstring ext = de.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (ext != L".nif") continue;
                    Entry e;
                    e.kind = EntryKind::File;
                    e.path = de.path().wstring();
                    e.name = de.path().filename().wstring();
                    files.push_back(std::move(e));
                }
            }
            std::sort(folders.begin(), folders.end(), lessNoCase);
            std::sort(files.begin(), files.end(), lessNoCase);
            m_entries.insert(m_entries.end(), std::make_move_iterator(folders.begin()),
                             std::make_move_iterator(folders.end()));
            m_entries.insert(m_entries.end(), std::make_move_iterator(files.begin()),
                             std::make_move_iterator(files.end()));
        }
    }

    // Queue the new folder's files for the background worker.
    EnqueuePending();

    // Center the highlighted file (card sizes are provisional until thumbnails
    // render, but a re-list is followed by the pane's selection so this lands
    // close; a later same-folder pick re-centers precisely).
    CenterCurrentFile();

    // Presence (and thus our measured extent) may have changed, so relayout.
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void ThumbnailStrip::EnqueuePending()
{
    // Submit every not-yet-rendered .nif entry to the shared pool (skipped
    // while disabled - the loader idles when the strip is off).
    if (!m_enabled || !m_manager)
        return;
    ResourceManager* const mgr = m_manager;
    ThumbnailStrip* const self = this;
    const std::uint64_t gen = m_gen;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
    {
        const Entry& e = m_entries[i];
        if (e.kind != EntryKind::File || e.rendered)
            continue;
        const std::size_t index = i;
        const std::wstring path = e.path; // copy: the job must not touch m_entries
        mgr->Submit(ResourceManager::Priority::Thumbnail, { self, gen },
            [mgr, self, gen, index, path]()
            {
                // Pool thread: self-contained parse (no `self` dereference).
                auto parsed = std::make_shared<ParsedThumb>();
                parsed->generation = gen;
                parsed->index = index;
                BuildParsedThumb(mgr, path, *parsed);
                // UI apply, delivered only while this strip's gen is current.
                mgr->PostCompletion({ self, gen },
                    [self, parsed]() { self->AcceptParsed(parsed); });
            });
    }
}

void ThumbnailStrip::SetFixedExtent(float extent)
{
    extent = std::clamp(extent, kMinExtent, kMaxExtent);
    if (extent == m_fixedExtent)
        return;
    m_fixedExtent = extent;
    m_scroll = 0.0f; // card geometry changed; avoid a now-invalid scroll offset
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

std::wstring ThumbnailStrip::StepFile(int delta) const
{
    // File entries in display order (folders/".." are not stepped through).
    std::vector<std::size_t> files;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].kind == EntryKind::File)
            files.push_back(i);
    if (files.empty())
        return std::wstring();

    // Locate the currently-highlighted file among them.
    int cur = -1;
    for (std::size_t k = 0; k < files.size(); ++k)
        if (m_entries[files[k]].path == m_currentFile)
        {
            cur = static_cast<int>(k);
            break;
        }

    const int n = static_cast<int>(files.size());
    int next;
    if (cur < 0)
        next = (delta >= 0) ? 0 : n - 1; // nothing highlighted -> first / last
    else
        next = ((cur + delta) % n + n) % n; // wrap around
    return m_entries[files[static_cast<std::size_t>(next)]].path;
}

std::wstring ThumbnailStrip::EdgeFile(bool last) const
{
    if (last)
    {
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
            if (it->kind == EntryKind::File)
                return it->path;
    }
    else
    {
        for (const Entry& e : m_entries)
            if (e.kind == EntryKind::File)
                return e.path;
    }
    return std::wstring();
}

void ThumbnailStrip::NavigateUp()
{
    // The ".." tile (when present) carries the parent directory. NavigateTo
    // takes its args by value, so passing the entry's own path is safe.
    if (!m_entries.empty() && m_entries.front().kind == EntryKind::Up)
        NavigateTo(m_entries.front().path, m_currentFile);
}

FD2D::Size ThumbnailStrip::Measure(FD2D::Size available)
{
    if (!ShouldShow())
    {
        m_desired = { 0.0f, 0.0f };
        return m_desired;
    }
    m_desired = m_horizontal ? FD2D::Size { available.w, m_fixedExtent }
                             : FD2D::Size { m_fixedExtent, available.h };
    return m_desired;
}

void ThumbnailStrip::BuildParsedThumb(ResourceManager* manager, const std::wstring& path,
                                     ParsedThumb& out)
{
    // Pool thread: parse + build are free of shared state. The shared_ptr doc
    // must outlive the meshes, which borrow its geometry. STATIC on purpose -
    // it never touches the strip, which may be destroyed while this runs.
    // The parse goes through the manager's shared NifCache, so a file opened in
    // a pane (or listed in several strips) is parsed exactly once and reused.
    std::shared_ptr<const NifDocument> doc = manager->GetOrParseNif(path);
    if (!doc)
    {
        out.failed = true;
        return;
    }
    // SceneBuilder excludes hidden meshes by default, so the bounds below
    // already ignore them.
    std::vector<RenderMesh> meshes = SceneBuilder::build(*doc);
    Vector3 minB(1e9f, 1e9f, 1e9f), maxB(-1e9f, -1e9f, -1e9f);
    bool any = false;
    for (const RenderMesh& mesh : meshes)
    {
        if (!mesh.geometry) continue;
        for (const Vector3& p : mesh.geometry->positions)
        {
            Vector3 wp = mesh.worldTransform * p;
            minB.boundMin(wp); maxB.boundMax(wp); any = true;
        }
    }
    if (meshes.empty() || !any)
    {
        out.failed = true;
        return;
    }
    ComputeThumbFraming(meshes, minB, maxB, out);
    out.meshes = std::move(meshes);
    out.doc = std::move(doc);
}

void ThumbnailStrip::AcceptParsed(std::shared_ptr<ParsedThumb> parsed)
{
    // UI thread (a manager completion, already generation-checked so this strip
    // is current). Queue it for OnRenderD3D's immediate-context render.
    if (!parsed)
        return;
    m_ready.push_back(std::move(*parsed));
    Invalidate();
}

void ThumbnailStrip::ComputeThumbFraming(const std::vector<RenderMesh>& meshes,
                                         const Vector3& minB, const Vector3& maxB,
                                         ParsedThumb& out)
{
    const Vector3 center = (minB + maxB) * 0.5f;
    const float radius = (std::max)((maxB - minB).length() * 0.5f, 1.0f);

    // Orbit view (gentle downward tilt) turned a little off-axis (yaw) so the
    // thumbnail is a slight 3/4 view rather than dead-on frontal.
    Camera cam;
    cam.frame(center, radius);
    cam.setOrbit(Camera::kDefaultYaw - kYawRad, Camera::kDefaultPitch);
    const Matrix4 view = cam.viewMatrix();

    // Tight bounds of the (non-hidden) geometry in the yawed view.
    float vminX = 1e9f, vminY = 1e9f, vminZ = 1e9f;
    float vmaxX = -1e9f, vmaxY = -1e9f, vmaxZ = -1e9f;
    for (const RenderMesh& mesh : meshes)
    {
        if (!mesh.geometry) continue;
        for (const Vector3& p : mesh.geometry->positions)
        {
            const Vector3 vp = view * (mesh.worldTransform * p);
            vminX = (std::min)(vminX, vp[0]); vmaxX = (std::max)(vmaxX, vp[0]);
            vminY = (std::min)(vminY, vp[1]); vmaxY = (std::max)(vmaxY, vp[1]);
            vminZ = (std::min)(vminZ, vp[2]); vmaxZ = (std::max)(vmaxZ, vp[2]);
        }
    }

    const float cx = (vminX + vmaxX) * 0.5f, cy = (vminY + vmaxY) * 0.5f;
    const float vw = vmaxX - vminX, vh = vmaxY - vminY;
    const float margin = kThumbMarginFrac * (std::max)(vw, vh);
    float halfW = vw * 0.5f + margin;
    float halfH = vh * 0.5f + margin;
    // Cap the aspect by widening the SHORT side's margin, keeping the model
    // centered (so left==right and top==bottom margins).
    const float aspect0 = halfW / (std::max)(halfH, 1e-4f);
    if (aspect0 > kMaxAspect)      halfH = halfW / kMaxAspect;
    else if (aspect0 < kMinAspect) halfW = halfH * kMinAspect;
    out.aspect = halfW / (std::max)(halfH, 1e-4f);

    const float l = cx - halfW, r = cx + halfW, b = cy - halfH, t = cy + halfH;
    const float zpad = 0.02f * (vmaxZ - vminZ) + 1.0f;
    const float n = (std::max)(vminZ - zpad, 0.01f);
    const float f = vmaxZ + zpad;

    // Off-center orthographic, left-handed, D3D depth [0,1], column-vector
    // (translation in column 3) to match Camera::projectionMatrix(). Starting
    // from the identity, only the six cells below differ.
    Matrix4 proj;
    proj(0, 0) = 2.0f / (r - l);  proj(0, 3) = -(r + l) / (r - l);
    proj(1, 1) = 2.0f / (t - b);  proj(1, 3) = -(t + b) / (t - b);
    proj(2, 2) = 1.0f / (f - n);  proj(2, 3) = -n / (f - n);

    out.view = view;
    out.proj = proj;
    out.eyePos = cam.eyePosition();
}

void ThumbnailStrip::RenderParsedThumb(Entry& e, ParsedThumb& pt)
{
    e.rendered = true;
    if (pt.failed || !pt.doc || pt.meshes.empty())
    {
        e.failed = true;
        return;
    }
    // Non-square target matching the worker's tight framing (fixed height,
    // width follows the aspect).
    const UINT h = kThumbPx;
    const UINT w = static_cast<UINT>(std::clamp(std::lround(kThumbPx * pt.aspect),
                                                48L, 512L));
    if (!m_thumbTarget.Resize(m_device, w, h, 1))
    {
        e.failed = true;
        return;
    }
    m_thumbCache.Clear();

    std::filesystem::path nifPath(e.path);
    TextureCache textures(m_textureRepository);
    textures.SetSynchronous(true); // one-shot render: decode pending placeholders now
    textures.SetNifDirectory(nifPath.has_parent_path() ? nifPath.parent_path().wstring() : std::wstring());

    RenderSettings s;
    s.view = pt.view;       // rolled view + tight ortho, computed on the worker
    s.proj = pt.proj;
    s.eyePos = pt.eyePos;
    s.brightness = 1.15f;   // thumbnails read a touch brighter than the panes
    s.showGrid = false;
    s.showAxes = false;
    s.clearColor = Color4 { 0.11f, 0.11f, 0.13f, 1.0f };

    m_renderDevice->RenderScene(m_thumbTarget, m_thumbCache, pt.meshes, s, &textures);

    // Copy the render into a persistent per-thumbnail texture (the reusable
    // target is about to be overwritten by the next thumbnail).
    const D3D11_TEXTURE2D_DESC td {
        .Width = w, .Height = h,
        .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> persistent;
    if (SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &persistent)) && m_thumbTarget.ColorTexture())
    {
        m_context->CopyResource(persistent.Get(), m_thumbTarget.ColorTexture());
        e.tex = std::move(persistent);
        e.aspect = pt.aspect; // drives this card's width in the layout
    }
    else
    {
        e.failed = true;
    }
}

void ThumbnailStrip::OnRenderD3D(ID3D11DeviceContext* context)
{
    // Off: the loader idles - no parsing/rendering happens.
    if (!m_enabled)
    {
        FD2D::Wnd::OnRenderD3D(context);
        return;
    }
    // Render a few of the pool's freshly parsed scenes per frame (RenderScene
    // needs the immediate context, so it must run here on the UI thread).
    // m_ready is delivered by manager completions (already generation-checked)
    // and cleared on NavigateTo, so entries here are always for this folder.
    if (m_renderDevice && m_renderDevice->IsInitialized() && m_device &&
        m_context && m_textureRepository)
    {
        int done = 0;
        while (done < kPerFrame && !m_ready.empty())
        {
            ParsedThumb pt = std::move(m_ready.front());
            m_ready.pop_front();
            if (pt.index >= m_entries.size() || m_entries[pt.index].kind != EntryKind::File)
                continue;
            RenderParsedThumb(m_entries[pt.index], pt);
            ++done;
        }
        // Rendering resized some cards (aspect-driven widths), which shifts the
        // highlighted card - re-center it while auto-centering is in effect, so
        // the selection stays put as the strip settles.
        if (done > 0 && m_autoCenter)
            CenterCurrentFile();
    }
    if (!m_ready.empty())
        Invalidate(); // keep draining next frame

    FD2D::Wnd::OnRenderD3D(context);
}

void ThumbnailStrip::EnsureBitmap(Entry& entry)
{
    if (entry.bitmap || !entry.tex || !m_backplate)
        return;
    ID2D1RenderTarget* rt = m_backplate->RenderTarget();
    if (!rt) return;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&dc))) || !dc) return;
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(entry.tex->QueryInterface(IID_PPV_ARGS(&surface)))) return;

    D2D1_BITMAP_PROPERTIES1 bp {};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    dc->GetDpi(&bp.dpiX, &bp.dpiY);
    (void)dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &entry.bitmap);
}

void ThumbnailStrip::EnsureTextFormat()
{
    if (m_textFormat) return;
    IDWriteFactory* dw = FD2D::Core::DWriteFactory();
    if (!dw) return;
    (void)dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"", &m_textFormat);
    if (m_textFormat)
    {
        m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(m_textFormat.Get(), &ellipsis)))
        {
            DWRITE_TRIMMING trim { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            m_textFormat->SetTrimming(&trim, ellipsis.Get());
        }
    }
}

std::wstring ThumbnailStrip::TooltipText() const
{
    // Backplate only queries this on the window its own deep hit-test placed
    // under the cursor (us), so derive the card straight from the live cursor
    // instead of a routed hover index (MouseMove may not reach a docked strip).
    if (!m_backplate)
        return std::wstring();
    POINT pt;
    if (!GetCursorPos(&pt) || !ScreenToClient(m_backplate->Window(), &pt))
        return std::wstring();
    const int card = CardAtPoint(pt);
    if (card >= 0 && card < static_cast<int>(m_entries.size()))
        return m_entries[static_cast<std::size_t>(card)].path;
    return std::wstring();
}

void ThumbnailStrip::ClampScroll()
{
    const D2D1_RECT_F r = LayoutRect();
    const float viewMain = m_horizontal ? (r.right - r.left) : (r.bottom - r.top);
    const float maxScroll = (std::max)(0.0f, ContentExtent() - viewMain);
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
}

void ThumbnailStrip::CenterCurrentFile()
{
    if (m_currentFile.empty() || m_entries.empty())
        return;
    // Locate the highlighted file's card.
    int idx = -1;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].kind == EntryKind::File && m_entries[i].path == m_currentFile)
        {
            idx = static_cast<int>(i);
            break;
        }
    if (idx < 0)
        return;
    m_autoCenter = true; // intent to keep centered; retried in OnRenderD3D as the layout settles

    const D2D1_RECT_F r = LayoutRect();
    const float viewMain = m_horizontal ? (r.right - r.left) : (r.bottom - r.top);
    if (viewMain <= 1.0f)
        return; // layout not established yet - OnRenderD3D re-centers once it is

    const float content = ContentExtent();
    const float maxScroll = (std::max)(0.0f, content - viewMain);
    const float cardOff = CardOffset(static_cast<std::size_t>(idx)); // content-space start
    const float cardExt = CardExtent(static_cast<std::size_t>(idx));
    const float edgeZone = 0.5f * viewMain;

    // Center the card, but snap to the ends for items within half a viewport of
    // either edge (matches FICture2's EnsureCentered - no dead space at the ends).
    float target;
    if (cardOff <= edgeZone)
        target = 0.0f;
    else if (cardOff + cardExt >= content - edgeZone)
        target = maxScroll;
    else
        target = (cardOff + cardExt * 0.5f) - viewMain * 0.5f;

    m_scroll = std::clamp(target, 0.0f, maxScroll);
    Invalidate();
}

void ThumbnailStrip::DrawFolderIcon(ID2D1RenderTarget* target, const D2D1_RECT_F& rc, bool up) const
{
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.16f, 0.19f), &brush)))
        target->FillRectangle(rc, brush.Get());

    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    const float cx = (rc.left + rc.right) * 0.5f, cy = (rc.top + rc.bottom) * 0.5f;
    if (up)
    {
        // Up chevron for the ".." tile.
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.76f, 0.82f), &brush)))
        {
            const float s = (std::min)(w, h) * 0.26f;
            target->DrawLine({ cx - s, cy + s * 0.55f }, { cx, cy - s * 0.75f }, brush.Get(), 3.5f);
            target->DrawLine({ cx, cy - s * 0.75f }, { cx + s, cy + s * 0.55f }, brush.Get(), 3.5f);
        }
    }
    else
    {
        // Folder glyph: a body rect with a small tab on top-left.
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.83f, 0.70f, 0.36f), &brush)))
        {
            const float fw = w * 0.52f, fh = h * 0.36f;
            const float l = cx - fw * 0.5f, t = cy - fh * 0.42f;
            const D2D1_RECT_F tab { l, t - fh * 0.28f, l + fw * 0.42f, t + 1.0f };
            const D2D1_RECT_F body { l, t, l + fw, t + fh };
            target->FillRectangle(tab, brush.Get());
            target->FillRectangle(body, brush.Get());
        }
    }
}

void ThumbnailStrip::OnRender(ID2D1RenderTarget* target)
{
    if (!target || !ShouldShow())
        return;
    EnsureTextFormat();

    const D2D1_RECT_F r = LayoutRect();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;

    // Strip background + a hairline separator on the edge facing the content
    // (right when docked left, top when docked bottom).
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.12f), &brush)))
        target->FillRectangle(r, brush.Get());
    // Separator on the inner edge; it thickens + brightens into a drag grip
    // while the cursor is over it or a resize is in progress.
    const bool gripActive = m_gripHover || m_resizing;
    const float sepW = gripActive ? 3.0f : 1.0f;
    const D2D1_COLOR_F sepColor = gripActive ? D2D1::ColorF(0.42f, 0.58f, 0.82f)
                                             : D2D1::ColorF(0.28f, 0.29f, 0.33f);
    if (SUCCEEDED(target->CreateSolidColorBrush(sepColor, &brush)))
    {
        const D2D1_RECT_F sep = m_horizontal ? D2D1::RectF(r.left, r.top, r.right, r.top + sepW)
                                             : D2D1::RectF(r.right - sepW, r.top, r.right, r.bottom);
        target->FillRectangle(sep, brush.Get());
        // A short centered grip handle to hint the edge is draggable.
        if (gripActive && SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.85f, 1.0f), &brush)))
        {
            if (m_horizontal)
            {
                const float cx = (r.left + r.right) * 0.5f;
                target->FillRectangle(D2D1::RectF(cx - 18.0f, r.top, cx + 18.0f, r.top + sepW), brush.Get());
            }
            else
            {
                const float cy = (r.top + r.bottom) * 0.5f;
                target->FillRectangle(D2D1::RectF(r.right - sepW, cy - 18.0f, r.right, cy + 18.0f), brush.Get());
            }
        }
    }

    target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);

    // Header count (vertical mode only - the horizontal strip has no room).
    if (m_textFormat)
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!m_horizontal && m_textFormat && !m_folder.empty() &&
        SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.66f, 0.72f), &brush)))
    {
        // Current folder name, so the strip reads as "the active pane's folder".
        const std::wstring hdr = std::filesystem::path(m_folder).filename().wstring();
        target->DrawTextW(hdr.c_str(), static_cast<UINT32>(hdr.size()), m_textFormat.Get(),
            D2D1::RectF(r.left + kPad, r.top + 6.0f, r.right - kPad, r.top + kHeaderH),
            brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const float thumb = ThumbSide();

    // Card labels are centered under the thumbnail; long names ellipsize (the
    // format carries character-granularity trimming), full path on hover.
    if (m_textFormat)
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // Cards vary in size, so accumulate the offset along the scroll axis.
    float cursor = LeadGutter() - m_scroll;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
    {
        Entry& e = m_entries[i];
        const float ext = CardExtent(i);
        const float pos = cursor;
        cursor += ext;

        D2D1_RECT_F card, thumbRect;
        if (m_horizontal)
        {
            const float left = r.left + pos;
            if (left + ext < r.left || left > r.right)
                continue;
            const float thumbW = ext - kPad * 2.0f; // thumb*aspect (square for tiles)
            card = { left, r.top + 1.0f, left + ext, r.bottom };
            thumbRect = { left + kPad, r.top + kPad, left + kPad + thumbW, r.top + kPad + thumb };
        }
        else
        {
            const float top = r.top + pos;
            if (top + ext < r.top || top > r.bottom)
                continue;
            card = { r.left + 2.0f, top, r.right - 2.0f, top + ext };
            thumbRect = { r.left + kPad, top + kPad, r.left + kPad + thumb, top + kPad + thumb };
        }

        if (static_cast<int>(i) == m_hoverCard &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.17f, 0.19f, 0.24f), &brush)))
            target->FillRectangle(card, brush.Get());

        if (e.kind != EntryKind::File)
        {
            // Folder / ".." navigation tile.
            DrawFolderIcon(target, thumbRect, e.kind == EntryKind::Up);
        }
        else
        {
            EnsureBitmap(e);
            if (e.bitmap)
            {
                target->DrawBitmap(e.bitmap.Get(), thumbRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            else
            {
                const D2D1_COLOR_F c = e.failed ? D2D1::ColorF(0.28f, 0.14f, 0.14f)
                                                : D2D1::ColorF(0.14f, 0.14f, 0.17f);
                if (SUCCEEDED(target->CreateSolidColorBrush(c, &brush)))
                    target->FillRectangle(thumbRect, brush.Get());
            }
            // Accent border on the active pane's current file.
            if (!m_currentFile.empty() && e.path == m_currentFile &&
                SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.62f, 0.96f), &brush)))
            {
                const D2D1_RECT_F b { thumbRect.left - 1.5f, thumbRect.top - 1.5f,
                                      thumbRect.right + 1.5f, thumbRect.bottom + 1.5f };
                target->DrawRectangle(b, brush.Get(), 2.5f);
            }
        }

        if (m_textFormat &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.82f, 0.84f, 0.88f), &brush)))
        {
            const D2D1_RECT_F lbl { thumbRect.left, thumbRect.bottom + 2.0f,
                                    thumbRect.right, thumbRect.bottom + 2.0f + kLabelH };
            target->DrawTextW(e.name.c_str(), static_cast<UINT32>(e.name.size()), m_textFormat.Get(),
                lbl, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    target->PopAxisAlignedClip();
}

bool ThumbnailStrip::InResizeGrip(const POINT& pt) const
{
    if (!ShouldShow())
        return false;
    const D2D1_RECT_F r = LayoutRect();
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    // The grip is the band on the strip's inner edge (top when docked at the
    // bottom, right when docked on the left).
    if (m_horizontal)
        return x >= r.left && x <= r.right && y >= r.top && y <= r.top + kGripThickness;
    return y >= r.top && y <= r.bottom && x >= r.right - kGripThickness && x <= r.right;
}

int ThumbnailStrip::CardAtPoint(const POINT& pt) const
{
    const D2D1_RECT_F r = LayoutRect();
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    if (x < r.left || x > r.right || y < r.top || y > r.bottom)
        return -1;

    // Content-space position along the scroll axis (undo the scroll offset).
    float local;
    if (m_horizontal)
        local = (x - r.left) + m_scroll;
    else
    {
        if (y < r.top + kHeaderH) return -1; // header row
        local = (y - r.top) + m_scroll;
    }
    // Walk the variable-width cards.
    float off = LeadGutter();
    for (std::size_t i = 0; i < m_entries.size(); ++i)
    {
        const float ext = CardExtent(i);
        if (local >= off && local < off + ext)
            return static_cast<int>(i);
        off += ext;
    }
    return -1;
}

bool ThumbnailStrip::OnInputEvent(const FD2D::InputEvent& event)
{
    using FD2D::InputEventType;
    using FD2D::MouseButton;

    const D2D1_RECT_F r = LayoutRect();
    auto inStrip = [&](const POINT& p) {
        return ShouldShow() && p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
    };

    switch (event.type)
    {
    case InputEventType::MouseWheel:
        if (event.hasPoint && inStrip(event.point))
        {
            // Wheel scrolls the strip's main axis (down/away = later items).
            // Scroll ~half a typical card per wheel notch. Manual scroll takes
            // over from auto-centering.
            m_scroll -= static_cast<float>(event.wheelDelta) / 120.0f * (ThumbSide() + kPad * 2.0f) * 0.5f;
            m_autoCenter = false;
            ClampScroll();
            Invalidate();
            return true;
        }
        break;

    case InputEventType::MouseMove:
    {
        if (!event.hasPoint) break;
        if (m_resizing)
        {
            // Drag the inner edge: toward the 3D view grows the strip.
            const float cur = m_horizontal ? static_cast<float>(event.point.y)
                                           : static_cast<float>(event.point.x);
            const float delta = m_horizontal ? (m_dragStartMouse - cur) : (cur - m_dragStartMouse);
            const float newExt = std::clamp(m_dragStartExtent + delta, kMinExtent, kMaxExtent);
            if (newExt != m_fixedExtent)
            {
                SetFixedExtent(newExt);
                if (m_onResize)
                    m_onResize(newExt, /*committed=*/false); // live-mirror onto the other panes
            }
            return true;
        }
        const bool grip = InResizeGrip(event.point);
        if (grip != m_gripHover) { m_gripHover = grip; Invalidate(); }
        const int hit = (!grip && inStrip(event.point)) ? CardAtPoint(event.point) : -1;
        if (hit != m_hoverCard) { m_hoverCard = hit; Invalidate(); }
        break;
    }

    case InputEventType::SetCursor:
    {
        // Resize cursor while over the grip or dragging it.
        POINT pt {};
        const bool overGrip = m_backplate && GetCursorPos(&pt) &&
                              ScreenToClient(m_backplate->Window(), &pt) && InResizeGrip(pt);
        if (m_resizing || overGrip)
        {
            SetCursor(LoadCursor(nullptr, m_horizontal ? IDC_SIZENS : IDC_SIZEWE));
            return true;
        }
        break;
    }

    case InputEventType::MouseDown:
        if (event.button == MouseButton::Left && event.hasPoint)
        {
            // The resize grip takes priority over a card click.
            if (InResizeGrip(event.point) && m_backplate)
            {
                m_resizing = true;
                m_dragStartMouse = m_horizontal ? static_cast<float>(event.point.y)
                                                : static_cast<float>(event.point.x);
                m_dragStartExtent = m_fixedExtent;
                SetCapture(m_backplate->Window());
                Invalidate();
                return true;
            }
            if (inStrip(event.point))
            {
                const int hit = CardAtPoint(event.point);
                if (hit >= 0)
                {
                    RequestFocus();
                    const Entry& e = m_entries[static_cast<std::size_t>(hit)];
                    if (e.kind == EntryKind::File)
                    {
                        // Load the sibling into the active pane (owner's handler).
                        if (m_onActivated)
                            m_onActivated(e.path);
                    }
                    else
                    {
                        // Folder / ".." tile: navigate the strip in place, keeping
                        // the current-file highlight (matches only if it reappears).
                        NavigateTo(e.path, m_currentFile);
                    }
                    return true;
                }
            }
        }
        break;

    case InputEventType::MouseUp:
        if (m_resizing && event.button == MouseButton::Left)
        {
            m_resizing = false;
            ReleaseCapture();
            if (m_onResize)
                m_onResize(m_fixedExtent, /*committed=*/true); // persist the final size
            Invalidate();
            return true;
        }
        break;

    case InputEventType::CaptureChanged:
        if (m_resizing)
        {
            m_resizing = false;
            if (m_onResize)
                m_onResize(m_fixedExtent, /*committed=*/true);
            Invalidate();
        }
        break;

    default:
        break;
    }

    return FD2D::Wnd::OnInputEvent(event);
}

} // namespace nsk
