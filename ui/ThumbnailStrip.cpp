#include "ThumbnailStrip.h"

#include "ImageAlphaPresentation.h"
#include "../core/NifDocument.h"
#include "../core/SceneBuilder.h"
#include "../core/Camera.h"
#include "../core/ResourceManager.h"
#include "../render/TextureCache.h"

#include <Backplate.h>
#include <Core.h>
#include <Util.h>
#include <VirtualPath.h>        // Floar: parse a folder string into a VirtualPath
#include <VirtualFileSystem.h>  // Floar: list a folder OR a BSA/BA2's contents

#include "ImageCore/ImageDecodeDispatcher.h" // which extensions are textures
#include "ImageCore/ImageLoader.h"           // decode image thumbnails
#include "ImageCore/ImageRequest.h"
#include "ImageCore/DecodedImage.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace nsk
{

struct ThumbnailStrip::AsyncState
{
    ThumbnailStrip* owner = nullptr; // UI-thread access only
};

struct ThumbnailStrip::ImageThumbRequest
{
    enum class Phase
    {
        Starting,
        Active,
        CallbackClaimed,
        Cancelled
    };

    ImageThumbRequest(std::uint64_t gen, std::size_t entryIndex, std::wstring source)
        : generation(gen), index(entryIndex), path(std::move(source))
    {
    }

    void PublishHandle(ImageCore::ImageHandle newHandle)
    {
        bool cancelNow = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (phase == Phase::Starting)
            {
                handle = newHandle;
                phase = Phase::Active;
            }
            else if (phase == Phase::Cancelled)
            {
                cancelNow = newHandle != 0;
            }
        }
        if (cancelNow)
        {
            ImageCore::ImageLoader::Instance().Cancel(newHandle);
        }
    }

    bool ClaimCallback()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (phase == Phase::Cancelled)
        {
            return false;
        }
        phase = Phase::CallbackClaimed;
        handle = 0;
        return true;
    }

    bool FailedToStart()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return phase == Phase::Active && handle == 0;
    }

    void Cancel()
    {
        ImageCore::ImageHandle cancelHandle = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (phase == Phase::Cancelled)
            {
                return;
            }
            if (phase == Phase::Active)
            {
                cancelHandle = handle;
            }
            phase = Phase::Cancelled;
            handle = 0;
        }
        if (cancelHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(cancelHandle);
        }
    }

    const std::uint64_t generation;
    const std::size_t index;
    const std::wstring path;
    std::mutex mutex;
    Phase phase { Phase::Starting };
    ImageCore::ImageHandle handle { 0 };
};

namespace
{
    // Visits each vertex of `mesh` that a triangle actually references (world-
    // transformed), calling `fn(worldPos)` for each - not every entry in the
    // positions array. Some legacy-format shapes carry orphan trailing
    // vertices no triangle indexes, left at raw/unskinned coordinates far
    // from the rest of the mesh (see NifViewport.cpp's AccumulateTriBounds
    // for the full story); walking every stored position lets such invisible
    // junk vertices blow up thumbnail framing the same way it blew up the
    // main viewport's camera.
    template <typename Fn>
    void ForEachTriVertex(const RenderMesh& mesh, Fn&& fn)
    {
        if (!mesh.geometry) return;
        const auto& positions = mesh.geometry->positions;
        for (const Triangle& tri : mesh.geometry->triangles)
        {
            for (const std::uint16_t idx : { tri.v1(), tri.v2(), tri.v3() })
            {
                if (idx < positions.size())
                    fn(mesh.worldTransform * positions[idx]);
            }
        }
    }

    constexpr float kPad = 8.0f;
    constexpr float kLabelH = 20.0f;
    constexpr float kGripThickness = 8.0f;  // drag-to-resize band on the inner edge
    constexpr float kScrollBarMargin = 8.0f;
    constexpr float kScrollBarHeight = 8.0f;
    constexpr float kScrollBarBottom = 2.0f;
    constexpr float kScrollThumbMin = 28.0f;
    constexpr float kMinExtent = 110.0f;    // resize clamp (see SetFixedExtent)
    constexpr float kMaxExtent = 360.0f;
    constexpr float kHeaderH = 26.0f;      // count header (vertical mode only)
    constexpr UINT kThumbPx = 168;         // offscreen render HEIGHT (width follows aspect)
    constexpr int kPerFrame = 2;           // thumbnails generated per frame
    constexpr float kYawRad = 0.1745f;     // ~10 deg yaw (3/4 view) so thumbnails aren't dead-on
    constexpr float kThumbMarginFrac = 0.06f; // equal margin, fraction of the larger extent
    constexpr float kMinAspect = 0.45f;    // clamp card w/h so cards stay reasonable
    constexpr float kMaxAspect = 2.6f;

    // True when `extLower` (dotted, lowercase) is a texture ImageCore can decode.
    bool IsImageExt(const std::wstring& extLower)
    {
        if (extLower.empty())
            return false;
        for (const std::wstring& s : ImageCore::ImageDecodeDispatcher::GetSupportedExtensions())
            if (extLower == s)
                return true;
        return false;
    }
}

ThumbnailStrip::ThumbnailStrip(const std::wstring& name)
    : FD2D::Wnd(name),
      m_asyncState(std::make_shared<AsyncState>())
{
    m_asyncState->owner = this;
}

ThumbnailStrip::~ThumbnailStrip()
{
    m_asyncState->owner = nullptr;
    InvalidatePendingRequests(true);
    if (m_manager)
        m_manager->Cancel(m_asyncState.get());
}

void ThumbnailStrip::InvalidatePendingRequests(bool clearReady)
{
    for (Entry& entry : m_entries)
    {
        std::shared_ptr<ImageThumbRequest> request = std::move(entry.imageRequest);
        entry.pending = false;
        if (request)
        {
            request->Cancel();
        }
    }
    if (clearReady)
    {
        m_ready.clear();
    }
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
            m_gen = m_manager->BumpGeneration(m_asyncState.get());
        m_listing = false;
        InvalidatePendingRequests(true);
    }
    else
    {
        // A disabled strip may have cancelled an archive enumeration before it
        // produced entries. Re-list that folder; otherwise only the thumbnail
        // work needs to resume.
        if (m_entries.empty() && !m_folder.empty())
        {
            NavigateTo(m_folder, m_currentFile);
        }
        else
        {
            EnqueuePending();
        }
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
    const FD2D::GraphicsGeneration generation =
        backplate.GetGraphicsGeneration();
    m_boundDeviceGeneration = generation.device;
    m_boundTargetGeneration = generation.target;
}

void ThumbnailStrip::OnGraphicsInvalidated(
    FD2D::GraphicsInvalidationReason reason,
    const FD2D::GraphicsGeneration& generation)
{
    const bool targetChanged =
        generation.target != m_boundTargetGeneration;
    const bool deviceChanged =
        generation.device != m_boundDeviceGeneration;

    if (targetChanged || deviceChanged)
    {
        for (Entry& entry : m_entries)
        {
            entry.bitmap.Reset();
        }
    }
    if (deviceChanged)
    {
        m_device = nullptr;
        m_context = nullptr;
        m_thumbTarget = {};
        m_thumbCache.Clear();
        for (Entry& entry : m_entries)
        {
            entry.tex.Reset();
            if (entry.kind == EntryKind::File && !entry.isImage)
            {
                entry.rendered = false;
                entry.failed = false;
            }
        }
    }

    m_boundDeviceGeneration = generation.device;
    m_boundTargetGeneration = generation.target;
    FD2D::Wnd::OnGraphicsInvalidated(reason, generation);
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
    if (dir == m_folder && (m_listing || !m_entries.empty()))
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

bool ThumbnailStrip::ShowForFolder(const std::wstring& folder)
{
    if (folder.empty())
        return true;
    // List the folder/archive itself (not a file's parent), no highlight.
    return NavigateTo(folder, std::wstring());
}

std::wstring ThumbnailStrip::PickDefaultEntry()
{
    // A viewable file wins: hand its path back so the owner loads it. Entries
    // are ordered [Up?, folders..., files...], so the first File is the
    // alphabetically-first nif/texture.
    for (const Entry& e : m_entries)
        if (e.kind == EntryKind::File)
            return e.path;

    // No files here: highlight the first subfolder tile, or the ".." Up tile if
    // there is no subfolder, so the user can step in from a sensible cursor.
    int folderIdx = -1;
    int upIdx = -1;
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
    {
        if (m_entries[i].kind == EntryKind::Folder) { folderIdx = i; break; }
        if (m_entries[i].kind == EntryKind::Up) upIdx = i;
    }
    const int sel = (folderIdx >= 0) ? folderIdx : upIdx;
    if (sel >= 0)
    {
        m_selected = sel;
        m_autoCenter = true;
        CenterEntry(sel);
        Invalidate();
    }
    return std::wstring();
}

bool ThumbnailStrip::NavigateTo(std::wstring folder, std::wstring selectPath)
{
    // Cancel in-flight/queued parses from the previous folder: bump our
    // generation (the manager drops older jobs + results) and drop any parsed
    // scenes already queued for render.
    if (m_manager)
        m_gen = m_manager->BumpGeneration(m_asyncState.get());
    InvalidatePendingRequests(true);

    m_entries.clear();
    m_scroll = 0.0f;
    m_hoverCard = -1;
    m_selected = -1; // the listing changed; drop the keyboard selection cursor
    m_typeQuery.clear(); // and any in-progress type-to-select query
    m_folder = folder;
    m_currentFile = selectPath;
    m_thumbCache.Clear();
    m_listing = false;

    if (!folder.empty())
    {
        auto vpOpt = Floar::VirtualPath::Parse(folder);
        const bool isArchivePath = vpOpt && (vpOpt->IsArchiveFile() || vpOpt->IsInArchive());

        // Archive readers expose a flat entry table. Even with the reader cache,
        // finding one directory's immediate children walks that entire table
        // (about 95 ms for a 42k-entry BA2), so never do it on the UI thread.
        if (isArchivePath && m_manager)
        {
            m_listing = true;
            ResourceManager* const mgr = m_manager;
            std::shared_ptr<AsyncState> state = m_asyncState;
            const ResourceManager::Token token { state.get(), m_gen };
            const std::uint64_t generation = m_gen;
            mgr->Submit(
                ResourceManager::Priority::OtherPane,
                token,
                [mgr, state, token, generation,
                 folder = std::move(folder)]() mutable
                {
                    std::shared_ptr<DirectoryListing> listing =
                        BuildDirectoryListing(
                            generation,
                            std::move(folder));
                    mgr->PostCompletion(
                        token,
                        [state, listing = std::move(listing)]() mutable
                        {
                            if (ThumbnailStrip* owner = state->owner)
                            {
                                owner->ApplyDirectoryListing(
                                    std::move(listing),
                                    true);
                            }
                        });
                });

            if (FD2D::Backplate* bp = BackplateRef())
            {
                bp->RequestLayout();
            }
            Invalidate();
            return false;
        }
    }

    ApplyDirectoryListing(
        BuildDirectoryListing(m_gen, std::move(folder)),
        false);
    return true;
}

std::shared_ptr<ThumbnailStrip::DirectoryListing>
ThumbnailStrip::BuildDirectoryListing(
    std::uint64_t generation,
    std::wstring folder)
{
    auto listing = std::make_shared<DirectoryListing>();
    listing->generation = generation;
    listing->folder = std::move(folder);

    if (listing->folder.empty())
    {
        return listing;
    }

    // List through Floar's VFS so the strip descends into BSA/BA2/common
    // archives as if they were folders. The resulting paths remain VFS display
    // paths and are routed back through Floar when a member is opened.
    auto vpOpt = Floar::VirtualPath::Parse(listing->folder);
    if (!vpOpt)
    {
        return listing;
    }

    const Floar::VirtualPath vp = *vpOpt;
    const bool isArchivePath = vp.IsArchiveFile() || vp.IsInArchive();
    // IsDirectory itself scans archive entry tables, so archive paths go
    // directly to ListDirectory. Filesystem directories keep the cheap guard.
    if (!isArchivePath && !Floar::VirtualFileSystem::IsDirectory(vp))
    {
        return listing;
    }

    const Floar::VirtualPath parent = vp.GetParent();
    if (parent != vp)
    {
        Entry up;
        up.kind = EntryKind::Up;
        up.path = parent.wstring();
        up.name = L"..";
        up.rendered = true;
        listing->entries.push_back(std::move(up));
    }

    std::vector<Entry> folders;
    std::vector<Entry> files;
    for (const auto& de : Floar::VirtualFileSystem::ListDirectory(vp))
    {
        const bool isArchive = de.path.IsArchiveFile();
        if (de.isDirectory || isArchive)
        {
            Entry entry;
            entry.kind = EntryKind::Folder;
            entry.path = de.path.wstring();
            entry.name = de.path.GetFilename();
            entry.rendered = true;
            if (isArchive)
            {
                std::wstring ext = de.path.GetExtension();
                std::transform(
                    ext.begin(),
                    ext.end(),
                    ext.begin(),
                    [](wchar_t c)
                    {
                        return static_cast<wchar_t>(std::towlower(c));
                    });
                if (ext == L".bsa")
                {
                    entry.archiveKind = ArchiveKind::Bsa;
                }
                else if (ext == L".ba2")
                {
                    entry.archiveKind = ArchiveKind::Ba2;
                }
                else if (ext == L".zip")
                {
                    entry.archiveKind = ArchiveKind::Zip;
                }
                else if (ext == L".7z")
                {
                    entry.archiveKind = ArchiveKind::SevenZip;
                }
                else if (ext == L".rar")
                {
                    entry.archiveKind = ArchiveKind::Rar;
                }
                else
                {
                    entry.archiveKind = ArchiveKind::Other;
                }
            }
            folders.push_back(std::move(entry));
            continue;
        }

        std::wstring ext = de.path.GetExtension();
        std::transform(
            ext.begin(),
            ext.end(),
            ext.begin(),
            [](wchar_t c)
            {
                return static_cast<wchar_t>(std::towlower(c));
            });
        const bool isNif = ext == L".nif";
        const bool isImage = !isNif && IsImageExt(ext);
        if (!isNif && !isImage)
        {
            continue;
        }

        Entry entry;
        entry.kind = EntryKind::File;
        entry.isImage = isImage;
        entry.path = de.path.wstring();
        entry.name = de.path.GetFilename();
        files.push_back(std::move(entry));
    }

    auto lessNoCase = [](const Entry& a, const Entry& b)
    {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    };
    std::sort(folders.begin(), folders.end(), lessNoCase);
    std::sort(files.begin(), files.end(), lessNoCase);
    listing->entries.insert(
        listing->entries.end(),
        std::make_move_iterator(folders.begin()),
        std::make_move_iterator(folders.end()));
    listing->entries.insert(
        listing->entries.end(),
        std::make_move_iterator(files.begin()),
        std::make_move_iterator(files.end()));
    return listing;
}

void ThumbnailStrip::ApplyDirectoryListing(
    std::shared_ptr<DirectoryListing> listing,
    bool notifyCompletion)
{
    if (!listing ||
        listing->generation != m_gen ||
        listing->folder != m_folder)
    {
        return;
    }

    m_listing = false;
    m_entries = std::move(listing->entries);

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

    if (notifyCompletion && m_onListingCompleted)
    {
        m_onListingCompleted(m_folder);
    }
}

void ThumbnailStrip::EnqueuePending()
{
    // Submit every not-yet-rendered .nif entry to the shared pool (skipped
    // while disabled - the loader idles when the strip is off).
    if (!m_enabled || !m_manager)
        return;
    ResourceManager* const mgr = m_manager;
    const std::uint64_t gen = m_gen;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
    {
        Entry& e = m_entries[i];
        if (e.kind != EntryKind::File || e.rendered || e.pending)
            continue;
        const std::size_t index = i;
        const std::wstring path = e.path; // copy: the job must not touch m_entries
        e.pending = true;

        if (e.isImage)
        {
            // Submit directly to ImageCore. ResourceManager is used only to
            // marshal the completion back to the UI thread; no pool worker is
            // occupied waiting on ImageCore's scheduler.
            auto request = std::make_shared<ImageThumbRequest>(gen, index, path);
            e.imageRequest = request;
            std::shared_ptr<AsyncState> state = m_asyncState;
            const ResourceManager::Token token { state.get(), gen };

            ImageCore::ImageRequest imageRequest(path, ImageCore::ImagePurpose::Thumbnail);
            imageRequest.allowGpuCompressedDDS = false;
            imageRequest.srgb = true;
            imageRequest.targetSize = { 256.0f, 256.0f };
            const ImageCore::ImageHandle handle =
                ImageCore::ImageLoader::Instance().RequestDecoded(
                imageRequest,
                [mgr, state, request, token](HRESULT hr, ImageCore::DecodedImage img)
                {
                    if (!request->ClaimCallback())
                    {
                        return;
                    }

                    std::shared_ptr<std::vector<std::uint8_t>> pixels;
                    std::uint32_t w = 0, h = 0, pitch = 0;
                    if (SUCCEEDED(hr) && img.blocks && !img.blocks->empty() &&
                        img.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM)
                    {
                        // Resolve the same Auto policy as ImagePane and build a
                        // D2D PREMULTIPLIED presentation copy. The decoded source
                        // remains lossless and no per-tile override is applied.
                        const ImageAlphaInfo alpha = AlphaInfoFromDecodedImage(img);
                        const ImageCore::AlphaUsage usage = ResolveAlphaUsage(alpha);
                        std::vector<std::uint8_t> presentation =
                            BuildBgra8Presentation(img, usage);
                        if (!presentation.empty())
                        {
                            pixels = std::make_shared<std::vector<std::uint8_t>>(
                                std::move(presentation));
                            w = img.width;
                            h = img.height;
                            pitch = img.rowPitchBytes ? img.rowPitchBytes : img.width * 4;
                        }
                    }
                    mgr->PostCompletion(
                        token,
                        [state, request, pixels, w, h, pitch]()
                        {
                            if (ThumbnailStrip* owner = state->owner)
                            {
                                owner->AcceptImageThumb(request, pixels, w, h, pitch);
                            }
                        });
                });
            request->PublishHandle(handle);
            if (request->FailedToStart() && e.imageRequest == request)
            {
                e.imageRequest.reset();
                e.pending = false;
                e.rendered = true;
                e.failed = true;
            }
            continue;
        }

        std::shared_ptr<AsyncState> state = m_asyncState;
        const ResourceManager::Token token { state.get(), gen };
        ResourceResolver* const resolver = m_resolver; // stable, app-owned - safe to capture like mgr
        mgr->Submit(ResourceManager::Priority::Thumbnail, token,
            [mgr, resolver, state, gen, index, path, token]()
            {
                // Pool thread: self-contained parse (no strip dereference).
                auto parsed = std::make_shared<ParsedThumb>();
                parsed->generation = gen;
                parsed->index = index;
                BuildParsedThumb(mgr, resolver, path, *parsed);
                // UI apply, delivered only while this strip's gen is current.
                mgr->PostCompletion(
                    token,
                    [state, parsed]()
                    {
                        if (ThumbnailStrip* owner = state->owner)
                        {
                            owner->AcceptParsed(parsed);
                        }
                    });
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

void ThumbnailStrip::NavigateUp()
{
    // The ".." tile (when present) carries the parent directory. NavigateTo
    // takes its args by value, so passing the entry's own path is safe.
    if (!m_entries.empty() && m_entries.front().kind == EntryKind::Up)
        NavigateTo(m_entries.front().path, m_currentFile);
}

std::wstring ThumbnailStrip::StepSelection(int delta)
{
    if (m_entries.empty())
        return std::wstring();

    int cur = m_selected;
    if (cur < 0)
    {
        // No cursor yet: start from the highlighted current file if it's listed,
        // else a virtual slot so the first step lands on the first/last tile.
        for (std::size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].kind == EntryKind::File && m_entries[i].path == m_currentFile)
            {
                cur = static_cast<int>(i);
                break;
            }
        if (cur < 0)
            cur = (delta >= 0) ? -1 : static_cast<int>(m_entries.size());
    }

    const int next = std::clamp(cur + delta, 0, static_cast<int>(m_entries.size()) - 1);
    if (next == m_selected)
        return std::wstring(); // already at that end - no move
    m_selected = next;
    CenterEntry(m_selected);
    Invalidate();

    const Entry& e = m_entries[static_cast<std::size_t>(next)];
    return (e.kind == EntryKind::File) ? e.path : std::wstring();
}

int ThumbnailStrip::PagingStep(int direction) const
{
    if (m_entries.empty() || direction == 0)
    {
        return 1;
    }

    int current = m_selected;
    if (current < 0 && !m_currentFile.empty())
    {
        for (std::size_t i = 0; i < m_entries.size(); ++i)
        {
            if (m_entries[i].kind == EntryKind::File &&
                m_entries[i].path == m_currentFile)
            {
                current = static_cast<int>(i);
                break;
            }
        }
    }
    if (current < 0)
    {
        current = direction > 0 ? -1 : static_cast<int>(m_entries.size());
    }

    const D2D1_RECT_F r = LayoutRect();
    const float viewport = m_horizontal
        ? r.right - r.left
        : r.bottom - r.top;
    const float pageExtent = (std::max)(
        1.0f,
        viewport - LeadGutter() - kPad);

    float accumulated = 0.0f;
    int count = 0;
    for (int index = current + direction;
         index >= 0 && index < static_cast<int>(m_entries.size());
         index += direction)
    {
        const float extent = CardExtent(static_cast<std::size_t>(index));
        if (count > 0 && accumulated + extent > pageExtent)
        {
            break;
        }
        accumulated += extent;
        ++count;
    }
    return (std::max)(1, count);
}

std::wstring ThumbnailStrip::PageSelection(int direction)
{
    if (direction == 0)
    {
        return std::wstring();
    }
    const int normalizedDirection = direction < 0 ? -1 : 1;
    return StepSelection(
        normalizedDirection * PagingStep(normalizedDirection));
}

std::wstring ThumbnailStrip::EdgeSelection(bool last)
{
    if (m_entries.empty())
        return std::wstring();
    const int idx = last ? static_cast<int>(m_entries.size()) - 1 : 0;
    m_selected = idx;
    CenterEntry(idx);
    Invalidate();
    const Entry& e = m_entries[static_cast<std::size_t>(idx)];
    return (e.kind == EntryKind::File) ? e.path : std::wstring();
}

bool ThumbnailStrip::ActivateSelection()
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_entries.size()))
        return false;
    const Entry& e = m_entries[static_cast<std::size_t>(m_selected)];
    if (e.kind == EntryKind::File)
    {
        if (m_onActivated)
            m_onActivated(e.path); // load it into the pane (owner also mirrors via Sync Files)
        return true;
    }
    // Folder / "..": browse the strip into it. NavigateTo clears m_entries and
    // takes its args by value, so copy the target path out of the entry first.
    std::wstring target = e.path;
    NavigateTo(std::move(target), m_currentFile);
    return true;
}

std::wstring ThumbnailStrip::TypeToSelect(wchar_t ch)
{
    if (m_entries.empty())
        return std::wstring();

    constexpr unsigned long long kTypeResetMs = 800;
    const unsigned long long now = GetTickCount64();
    if (now - m_typeLastMs > kTypeResetMs)
        m_typeQuery.clear(); // idle gap -> start a fresh query
    m_typeLastMs = now;

    const wchar_t lower = static_cast<wchar_t>(std::towlower(ch));
    // Same single key pressed again cycles to the next match; any other key
    // extends the prefix and matches from the current selection.
    const bool cycle = (m_typeQuery.size() == 1 && m_typeQuery[0] == lower);
    if (!cycle)
        m_typeQuery.push_back(lower);

    auto matches = [](const std::wstring& name, const std::wstring& q)
    {
        if (q.size() > name.size())
            return false;
        for (std::size_t i = 0; i < q.size(); ++i)
            if (static_cast<wchar_t>(std::towlower(name[i])) != q[i])
                return false;
        return true;
    };

    const int n = static_cast<int>(m_entries.size());
    const int base = (m_selected >= 0) ? m_selected : -1;
    const int from = cycle ? (base + 1) : (base < 0 ? 0 : base);
    for (int i = 0; i < n; ++i)
    {
        const int idx = ((from + i) % n + n) % n;
        if (matches(m_entries[static_cast<std::size_t>(idx)].name, m_typeQuery))
        {
            m_selected = idx;
            CenterEntry(idx);
            Invalidate();
            const Entry& e = m_entries[static_cast<std::size_t>(idx)];
            return (e.kind == EntryKind::File) ? e.path : std::wstring();
        }
    }
    return std::wstring(); // no match: keep the query + current selection
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

void ThumbnailStrip::BuildParsedThumb(ResourceManager* manager, const ResourceResolver* resolver,
                                     const std::wstring& path, ParsedThumb& out)
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
    std::vector<RenderMesh> meshes = SceneBuilder::build(*doc, /*includeHidden=*/false, resolver);
    Vector3 minB(1e9f, 1e9f, 1e9f), maxB(-1e9f, -1e9f, -1e9f);
    bool any = false;
    for (const RenderMesh& mesh : meshes)
    {
        ForEachTriVertex(mesh, [&](const Vector3& wp)
        {
            minB.boundMin(wp); maxB.boundMax(wp); any = true;
        });
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
    if (!parsed || parsed->generation != m_gen || parsed->index >= m_entries.size())
        return;
    Entry& entry = m_entries[parsed->index];
    if (entry.kind != EntryKind::File || entry.isImage || !entry.pending)
        return;
    m_ready.push_back(std::move(*parsed));
    Invalidate();
}

void ThumbnailStrip::AcceptImageThumb(
    const std::shared_ptr<ImageThumbRequest>& request,
    std::shared_ptr<std::vector<std::uint8_t>> pixels,
    std::uint32_t w,
    std::uint32_t h,
    std::uint32_t pitch)
{
    // UI thread (generation-checked completion). Stage the decoded pixels; the
    // D2D pass (EnsureBitmap) turns them into the tile bitmap.
    if (!request || request->generation != m_gen || request->index >= m_entries.size())
        return;
    Entry& e = m_entries[request->index];
    if (!e.isImage || e.path != request->path || e.imageRequest != request)
        return;
    e.imageRequest.reset();
    e.pending = false;
    e.rendered = true; // decode attempted (success or fail) - don't re-enqueue
    if (pixels && !pixels->empty() && w > 0 && h > 0)
    {
        e.imgPixels = std::move(pixels);
        e.imgW = w;
        e.imgH = h;
        e.imgPitch = pitch;
        e.aspect = std::clamp(static_cast<float>(w) / static_cast<float>(h), kMinAspect, kMaxAspect);
        e.bitmap.Reset(); // rebuild from the new pixels on the next paint
    }
    else
    {
        e.failed = true;
    }
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout(); // the card aspect may have changed
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
        ForEachTriVertex(mesh, [&](const Vector3& wp)
        {
            const Vector3 vp = view * wp;
            vminX = (std::min)(vminX, vp[0]); vmaxX = (std::max)(vmaxX, vp[0]);
            vminY = (std::min)(vminY, vp[1]); vmaxY = (std::max)(vmaxY, vp[1]);
            vminZ = (std::min)(vminZ, vp[2]); vmaxZ = (std::max)(vmaxZ, vp[2]);
        });
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
    e.pending = false;
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
    textures.SetGame(
        m_resolver
            ? m_resolver->GameForNifPath(
                pt.doc->filePath(),
                pt.doc->bsVersion())
            : BethesdaGameFromBsVersion(
                pt.doc->bsVersion()));

    RenderSettings s;
    s.view = pt.view;       // rolled view + tight ortho, computed on the worker
    s.proj = pt.proj;
    s.eyePos = pt.eyePos;
    s.brightness = 1.15f;   // thumbnails read a touch brighter than the panes
    s.showGrid = false;
    s.showAxes = false;
    s.clearColor = Color4 { 0.11f, 0.11f, 0.13f, 1.0f };
    // Archive scan still running: an unresolved BSA-backed diffuse is "not
    // ready yet", not missing - suppress the magenta missing-texture marker
    // (same rule as the live viewport) and flag the entry so the post-scan
    // pass in OnRenderD3D retakes it with the real textures.
    const bool scanPending = m_resolver && !m_resolver->IsArchiveScanReady();
    s.texturesPending = scanPending;
    e.renderedWhilePending = scanPending;

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
        e.bitmap.Reset(); // a retake replaced the texture: rebuild the D2D view
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
    if (context && m_renderDevice && m_textureRepository && m_backplate)
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device)
        {
            const std::uint64_t deviceGeneration =
                m_backplate->GetGraphicsGeneration().device;
            std::string error;
            if (m_renderDevice->EnsureInitialized(
                    device.Get(),
                    context,
                    deviceGeneration,
                    &error))
            {
                m_textureRepository->BindDevice(
                    device.Get(),
                    deviceGeneration);
                m_device = device.Get();
                m_context = context;
                m_boundDeviceGeneration = deviceGeneration;
                EnqueuePending();
            }
        }
    }
    // Render a few of the pool's freshly parsed scenes per frame (RenderScene
    // needs the immediate context, so it must run here on the UI thread).
    // m_ready is delivered by manager completions (already generation-checked)
    // and cleared on NavigateTo, so entries here are always for this folder.
    if (m_renderDevice && m_renderDevice->IsInitialized() && m_device &&
        m_context && m_textureRepository)
    {
        // Post-scan healing: thumbnails rendered while the archive scan was
        // still running went out without their BSA-backed textures. Once the
        // scan lands, drop them back to un-rendered (the old texture stays on
        // screen until its retake arrives) and resubmit - the parse is a
        // NifCache hit, so the retake is cheap.
        if (m_resolver && m_resolver->IsArchiveScanReady())
        {
            bool stale = false;
            for (Entry& e : m_entries)
                if (e.renderedWhilePending)
                {
                    e.renderedWhilePending = false;
                    e.rendered = false;
                    stale = true;
                }
            if (stale)
                EnqueuePending();
        }
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
        {
            if (m_selected >= 0) CenterEntry(m_selected);
            else CenterCurrentFile();
        }
    }
    if (!m_ready.empty())
        Invalidate(); // keep draining next frame

    FD2D::Wnd::OnRenderD3D(context);
}

void ThumbnailStrip::EnsureBitmap(Entry& entry)
{
    if (entry.bitmap || !m_backplate)
        return;
    ID2D1RenderTarget* rt = m_backplate->RenderTarget();
    if (!rt) return;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&dc))) || !dc) return;

    // Image tile: build the bitmap from the decoded CPU BGRA8 pixels.
    if (entry.isImage)
    {
        if (!entry.imgPixels || entry.imgPixels->empty() || entry.imgW == 0 || entry.imgH == 0)
            return;
        D2D1_BITMAP_PROPERTIES1 bp {};
        bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        dc->GetDpi(&bp.dpiX, &bp.dpiY);
        const D2D1_SIZE_U size { entry.imgW, entry.imgH };
        (void)dc->CreateBitmap(size, entry.imgPixels->data(), entry.imgPitch, bp, &entry.bitmap);
        return;
    }

    if (!entry.tex) return;
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
    if (m_textFormat && m_archiveBadgeFormat)
    {
        return;
    }
    IDWriteFactory* dw = FD2D::Core::DWriteFactory();
    if (!dw)
    {
        return;
    }
    if (!m_textFormat)
    {
        (void)dw->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"",
            &m_textFormat);
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
    if (!m_archiveBadgeFormat)
    {
        (void)dw->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            11.0f,
            L"",
            &m_archiveBadgeFormat);
        if (m_archiveBadgeFormat)
        {
            m_archiveBadgeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            m_archiveBadgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_archiveBadgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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

bool ThumbnailStrip::HorizontalScrollBarRects(
    D2D1_RECT_F& track,
    D2D1_RECT_F& thumb) const
{
    if (!m_horizontal || !ShouldShow())
    {
        return false;
    }

    const D2D1_RECT_F r = LayoutRect();
    const float viewport = r.right - r.left;
    const float content = ContentExtent();
    if (viewport <= 0.0f || content <= viewport + 0.5f)
    {
        return false;
    }

    track = D2D1::RectF(
        r.left + kScrollBarMargin,
        r.bottom - kScrollBarBottom - kScrollBarHeight,
        r.right - kScrollBarMargin,
        r.bottom - kScrollBarBottom);
    const float trackWidth = track.right - track.left;
    if (trackWidth <= 0.0f)
    {
        return false;
    }

    const float thumbWidth = std::clamp(
        trackWidth * viewport / content,
        (std::min)(kScrollThumbMin, trackWidth),
        trackWidth);
    const float maxScroll = content - viewport;
    const float travel = trackWidth - thumbWidth;
    const float left = track.left +
        (maxScroll > 0.0f ? travel * m_scroll / maxScroll : 0.0f);
    thumb = D2D1::RectF(
        left,
        track.top,
        left + thumbWidth,
        track.bottom);
    return true;
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
    CenterEntry(idx);
}

void ThumbnailStrip::CenterEntry(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(m_entries.size()))
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

void ThumbnailStrip::DrawFolderIcon(
    ID2D1RenderTarget* target,
    const D2D1_RECT_F& rc,
    bool up,
    ArchiveKind archiveKind) const
{
    D2D1_COLOR_F background = D2D1::ColorF(0.15f, 0.16f, 0.19f);
    D2D1_COLOR_F accent = D2D1::ColorF(0.83f, 0.70f, 0.36f);
    const wchar_t* badge = nullptr;
    UINT32 badgeLength = 0;
    switch (archiveKind)
    {
    case ArchiveKind::Bsa:
        background = D2D1::ColorF(0.12f, 0.17f, 0.26f);
        accent = D2D1::ColorF(0.34f, 0.58f, 0.90f);
        badge = L"BSA";
        badgeLength = 3;
        break;
    case ArchiveKind::Ba2:
        background = D2D1::ColorF(0.18f, 0.14f, 0.27f);
        accent = D2D1::ColorF(0.62f, 0.45f, 0.88f);
        badge = L"BA2";
        badgeLength = 3;
        break;
    case ArchiveKind::Zip:
        background = D2D1::ColorF(0.10f, 0.21f, 0.23f);
        accent = D2D1::ColorF(0.25f, 0.70f, 0.76f);
        badge = L"ZIP";
        badgeLength = 3;
        break;
    case ArchiveKind::SevenZip:
        background = D2D1::ColorF(0.12f, 0.22f, 0.16f);
        accent = D2D1::ColorF(0.36f, 0.74f, 0.48f);
        badge = L"7Z";
        badgeLength = 2;
        break;
    case ArchiveKind::Rar:
        background = D2D1::ColorF(0.24f, 0.12f, 0.21f);
        accent = D2D1::ColorF(0.80f, 0.39f, 0.68f);
        badge = L"RAR";
        badgeLength = 3;
        break;
    case ArchiveKind::Other:
        background = D2D1::ColorF(0.23f, 0.17f, 0.11f);
        accent = D2D1::ColorF(0.82f, 0.56f, 0.28f);
        badge = L"ARC";
        badgeLength = 3;
        break;
    case ArchiveKind::None:
    default:
        break;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(background, &brush)))
    {
        target->FillRectangle(rc, brush.Get());
    }

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
    else if (archiveKind == ArchiveKind::None)
    {
        // Folder glyph: a body rect with a small tab on top-left.
        if (SUCCEEDED(target->CreateSolidColorBrush(accent, &brush)))
        {
            const float fw = w * 0.52f, fh = h * 0.36f;
            const float l = cx - fw * 0.5f, t = cy - fh * 0.42f;
            const D2D1_RECT_F tab { l, t - fh * 0.28f, l + fw * 0.42f, t + 1.0f };
            const D2D1_RECT_F body { l, t, l + fw, t + fh };
            target->FillRectangle(tab, brush.Get());
            target->FillRectangle(body, brush.Get());
        }
    }
    else
    {
        // Archive package: a rounded colored box with a zipper and an explicit
        // format badge. The silhouette and color both differ from directories,
        // while BSA/BA2/common archive formats remain distinguishable.
        const float boxW = w * 0.56f;
        const float boxH = h * 0.48f;
        const float left = cx - boxW * 0.5f;
        const float top = cy - boxH * 0.5f;
        const D2D1_ROUNDED_RECT package {
            { left, top, left + boxW, top + boxH },
            5.0f,
            5.0f
        };
        if (SUCCEEDED(target->CreateSolidColorBrush(accent, &brush)))
        {
            target->FillRoundedRectangle(package, brush.Get());
        }

        if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.08f, 0.09f, 0.11f, 0.72f),
            &brush)))
        {
            const float zipperW = (std::max)(2.0f, boxW * 0.055f);
            target->FillRectangle(
                D2D1::RectF(
                    cx - zipperW * 0.5f,
                    top,
                    cx + zipperW * 0.5f,
                    top + boxH * 0.48f),
                brush.Get());
            target->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        cx - boxW * 0.10f,
                        top + boxH * 0.43f,
                        cx + boxW * 0.10f,
                        top + boxH * 0.54f),
                    2.0f,
                    2.0f),
                brush.Get());
        }

        if (badge && m_archiveBadgeFormat &&
            SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(0.97f, 0.98f, 1.0f),
                &brush)))
        {
            target->DrawTextW(
                badge,
                badgeLength,
                m_archiveBadgeFormat.Get(),
                D2D1::RectF(
                    left + 2.0f,
                    top + boxH * 0.48f,
                    left + boxW - 2.0f,
                    top + boxH - 2.0f),
                brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

void ThumbnailStrip::DrawImageIcon(ID2D1RenderTarget* target, const D2D1_RECT_F& rc) const
{
    // A little photo glyph (frame + sun + mountain) on the image placeholder,
    // so a texture tile reads differently from a NIF tile before its decoded
    // thumbnail (later step) lands.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.50f, 0.72f, 0.78f), &brush)))
        return;
    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    const float cx = (rc.left + rc.right) * 0.5f, cy = (rc.top + rc.bottom) * 0.5f;
    const float s = (std::min)(w, h) * 0.28f;
    const D2D1_RECT_F frame { cx - s, cy - s, cx + s, cy + s };
    target->DrawRectangle(frame, brush.Get(), 2.0f);
    const D2D1_ELLIPSE sun { { cx + s * 0.38f, cy - s * 0.40f }, s * 0.16f, s * 0.16f };
    target->FillEllipse(sun, brush.Get());
    target->DrawLine({ cx - s, cy + s }, { cx - s * 0.15f, cy - s * 0.15f }, brush.Get(), 2.0f);
    target->DrawLine({ cx - s * 0.15f, cy - s * 0.15f }, { cx + s, cy + s }, brush.Get(), 2.0f);
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

    if (m_listing && m_textFormat &&
        SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.62f, 0.66f, 0.72f),
            &brush)))
    {
        constexpr wchar_t kLoadingText[] = L"Loading archive...";
        target->DrawTextW(
            kLoadingText,
            static_cast<UINT32>(
                sizeof(kLoadingText) / sizeof(kLoadingText[0]) - 1),
            m_textFormat.Get(),
            D2D1::RectF(
                r.left + kPad,
                r.top + kPad,
                r.right - kPad,
                r.bottom - kPad),
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

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

        // Keyboard-selected tile gets an accent-tinted card; plain hover is dimmer.
        if (static_cast<int>(i) == m_selected &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.24f, 0.40f), &brush)))
            target->FillRectangle(card, brush.Get());
        else if (static_cast<int>(i) == m_hoverCard &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.17f, 0.19f, 0.24f), &brush)))
            target->FillRectangle(card, brush.Get());

        if (e.kind != EntryKind::File)
        {
            // Folder, archive package, or ".." navigation tile.
            DrawFolderIcon(
                target,
                thumbRect,
                e.kind == EntryKind::Up,
                e.archiveKind);
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
                // Image tiles get a distinct teal placeholder (decoded thumbnails
                // land here later); NIF tiles are neutral, failures are reddish.
                const D2D1_COLOR_F c = e.isImage ? D2D1::ColorF(0.13f, 0.20f, 0.22f)
                                       : e.failed ? D2D1::ColorF(0.28f, 0.14f, 0.14f)
                                                  : D2D1::ColorF(0.14f, 0.14f, 0.17f);
                if (SUCCEEDED(target->CreateSolidColorBrush(c, &brush)))
                    target->FillRectangle(thumbRect, brush.Get());
                if (e.isImage)
                    DrawImageIcon(target, thumbRect);
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

        // Keyboard selection ring (any tile kind), brighter than the loaded-file
        // border so a selected folder/".." reads as "picked, press Enter".
        if (static_cast<int>(i) == m_selected &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.60f, 0.82f, 1.0f), &brush)))
        {
            const D2D1_RECT_F b { thumbRect.left - 2.0f, thumbRect.top - 2.0f,
                                  thumbRect.right + 2.0f, thumbRect.bottom + 2.0f };
            target->DrawRectangle(b, brush.Get(), 2.5f);
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

    D2D1_RECT_F scrollTrack {};
    D2D1_RECT_F scrollThumb {};
    if (HorizontalScrollBarRects(scrollTrack, scrollThumb))
    {
        const float radius = kScrollBarHeight * 0.5f;
        if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f),
            &brush)))
        {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(scrollTrack, radius, radius),
                brush.Get());
        }
        const D2D1_COLOR_F thumbColor =
            (m_scrollbarDragging || m_scrollbarHover)
            ? D2D1::ColorF(0.52f, 0.68f, 0.92f, 0.96f)
            : D2D1::ColorF(0.52f, 0.58f, 0.68f, 0.92f);
        if (SUCCEEDED(target->CreateSolidColorBrush(thumbColor, &brush)))
        {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(scrollThumb, radius, radius),
                brush.Get());
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
    auto inRect = [](const D2D1_RECT_F& rect, const POINT& p)
    {
        return p.x >= rect.left && p.x <= rect.right &&
               p.y >= rect.top && p.y <= rect.bottom;
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
        if (m_scrollbarDragging)
        {
            D2D1_RECT_F track {};
            D2D1_RECT_F thumb {};
            if (HorizontalScrollBarRects(track, thumb))
            {
                const float travel =
                    (track.right - track.left) -
                    (thumb.right - thumb.left);
                const float maxScroll = (std::max)(
                    0.0f,
                    ContentExtent() - (r.right - r.left));
                if (travel > 0.5f)
                {
                    m_scroll = m_scrollbarDragScroll +
                        (static_cast<float>(event.point.x) -
                         m_scrollbarDragMouse) /
                        travel * maxScroll;
                    m_autoCenter = false;
                    ClampScroll();
                    Invalidate();
                }
            }
            return true;
        }
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
        D2D1_RECT_F scrollTrack {};
        D2D1_RECT_F scrollThumb {};
        const bool hasScrollBar =
            HorizontalScrollBarRects(scrollTrack, scrollThumb);
        const bool scrollHover =
            hasScrollBar && inRect(scrollThumb, event.point);
        if (scrollHover != m_scrollbarHover)
        {
            m_scrollbarHover = scrollHover;
            Invalidate();
        }
        const bool overScrollBar =
            hasScrollBar && inRect(scrollTrack, event.point);
        const int hit = (!grip && !overScrollBar && inStrip(event.point))
            ? CardAtPoint(event.point)
            : -1;
        if (hit != m_hoverCard) { m_hoverCard = hit; Invalidate(); }
        break;
    }

    case InputEventType::SetCursor:
    {
        // Resize cursor while over the grip or dragging it.
        POINT pt {};
        const bool haveCursor =
            m_backplate &&
            GetCursorPos(&pt) &&
            ScreenToClient(m_backplate->Window(), &pt);
        const bool overGrip = haveCursor && InResizeGrip(pt);
        D2D1_RECT_F scrollTrack {};
        D2D1_RECT_F scrollThumb {};
        const bool overScrollBar =
            haveCursor &&
            HorizontalScrollBarRects(scrollTrack, scrollThumb) &&
            inRect(scrollTrack, pt);
        if (m_scrollbarDragging || overScrollBar)
        {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return true;
        }
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
            D2D1_RECT_F scrollTrack {};
            D2D1_RECT_F scrollThumb {};
            if (HorizontalScrollBarRects(scrollTrack, scrollThumb) &&
                inRect(scrollTrack, event.point))
            {
                RequestFocus();
                m_autoCenter = false;
                if (inRect(scrollThumb, event.point) && m_backplate)
                {
                    m_scrollbarDragging = true;
                    m_scrollbarDragMouse =
                        static_cast<float>(event.point.x);
                    m_scrollbarDragScroll = m_scroll;
                    SetCapture(m_backplate->Window());
                }
                else
                {
                    const float page =
                        (r.right - r.left) * 0.9f;
                    m_scroll += event.point.x < scrollThumb.left
                        ? -page
                        : page;
                    ClampScroll();
                }
                Invalidate();
                return true;
            }
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
                    m_selected = hit; // keep the keyboard cursor in sync with clicks
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
        if (m_scrollbarDragging && event.button == MouseButton::Left)
        {
            m_scrollbarDragging = false;
            ReleaseCapture();
            Invalidate();
            return true;
        }
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
        if (m_scrollbarDragging)
        {
            m_scrollbarDragging = false;
            Invalidate();
        }
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
