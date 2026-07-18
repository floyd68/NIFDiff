// TextureRepository.h - process-wide texture pool shared by every viewport.
//
// Each NifViewport used to own the full DDS-decode + SRV cache, so a
// multi-pane compare of same-named NIFs (this tool's primary use case)
// re-read, re-decoded and re-uploaded every shared texture once per pane -
// and the complex-material probes re-read the same DDS twice more on top.
// This repository dedups by ResourceBytes::sourceKey, the identity of the
// RESOLVED bytes: panes whose same-named requests land on the same loose
// file / archive entry share one SRV, while requests that resolve
// differently (override vs vanilla) correctly stay distinct. The
// per-viewport TextureCache is now just a relative-path -> Entry memo on
// top of this.
//
// Single-threaded by design: everything runs on the UI thread (loads happen
// during render). All viewports share one D3D device (the backplate's), so
// sharing SRVs across them is safe.
#pragma once

#include "../core/ResourceManager.h" // shared load pool (async prefetch)
#include "../core/ResourceResolver.h"

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nsk
{

class TextureRepository
{
public:
    explicit TextureRepository(ResourceResolver* resolver)
        : m_resolver(resolver)
    {
    }

    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; // null when decode failed
        // Source DDS identity, recorded at load for the texture inspector:
        // pixel format, top-mip dimensions, mip count, and the resolved
        // source ("file:<abs>" / "bsa:<archive>|<entry>" - see
        // ResourceBytes::sourceKey) so mod-conflict resolution differences
        // are visible per pane.
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mipLevels = 0;
        std::string sourceKey;

        // Lazy combined complex-material probe (see EnsureCmProbe): both
        // verdicts come from one re-read of the source, at most once per
        // unique texture in the whole process.
        bool cmProbed = false;
        bool cmAlpha = false;   // coarsest-mip average alpha meaningfully < 1
        bool cmHeight = false;  // alpha actually varies (usable height field)

        // Resolution inputs recorded at load time so the lazy CM probe can
        // re-fetch the same bytes (Find with equal inputs is deterministic).
        std::string relativePath;
        std::wstring nifDirectory;
        BethesdaGame game { BethesdaGame::Unknown };

        // Async prefetch placeholder: this entry is pooled but its bytes are
        // still decoding on a worker (srv stays null until the completion fills
        // it in place). Meshes render untextured meanwhile, then pop in. Also
        // marks the source as in-flight so a second request coalesces onto it.
        bool pending = false;
    };

    // Bind to the live backplate device. A generation/device change drops all
    // pooled SRVs and invalidates async publications decoded for the old device.
    void BindDevice(ID3D11Device* device, std::uint64_t deviceGeneration);
    void InvalidateDevice(std::uint64_t nextDeviceGeneration);

    // Wire the shared load pool for async prefetch (registers a stable
    // cancellation token so completions always publish + clear in-flight).
    void SetResourceManager(ResourceManager* manager);

    // Resolve relativePath (against nifDirectory) and return the pooled
    // entry for the resolved source, loading/decoding/uploading it only if
    // this is the first time that source is seen. Returns nullptr when the
    // path does not resolve at all; an entry with a null srv means the
    // source resolved but failed to decode. Returned pointers stay valid
    // until Clear() (unordered_map nodes are stable).
    //
    // forceSyncPending: if the source is an async-prefetch placeholder still
    // decoding (srv null, pending), decode it in place now instead of handing
    // back the placeholder. A live viewport leaves it false (the texture pops
    // in when the async decode lands, next paint); a one-shot render that needs
    // the texture this instant (thumbnails) passes true.
    Entry* GetOrLoad(const std::string& relativePath, const std::wstring& nifDirectory,
                     BethesdaGame game,
                     bool forceSyncPending = false);

    // Computes both complex-material verdicts for the entry if not probed
    // yet (one DDS re-read, shared by the alpha and height questions).
    void EnsureCmProbe(Entry& entry);

    // Parallel prefetch, called on the UI thread with every texture path a
    // freshly built scene references: resolution stays on the calling
    // thread (the archive readers are not established as thread-safe, and
    // Find is ~0.3ms/path), then the unseen sources' file read + DDS parse
    // + SRV creation - the actual cost, ~8ms each - fan out across worker
    // threads (ID3D11Device resource creation is free-threaded). After
    // this, the first frame's GetOrLoad calls are pool hits. Failures are
    // left unpooled and take the normal lazy path.
    void Prefetch(const std::vector<std::string>& relativePaths,
                  const std::wstring& nifDirectory,
                  BethesdaGame game);

    // Async counterpart of Prefetch: resolve + dedup on the calling (UI)
    // thread, pool a null-srv placeholder per unseen source, then decode+upload
    // each on the shared pool (IoGate-gated) and fill the placeholder's srv in
    // place from the completion. The UI never blocks on the decode; textures
    // pop in as they finish. Falls back to synchronous Prefetch when no manager
    // is wired (e.g. tests). Safe to call repeatedly - in-flight/pooled sources
    // are skipped.
    void PrefetchAsync(const std::vector<std::string>& relativePaths,
                       const std::wstring& nifDirectory,
                       BethesdaGame game);

    void Clear();

private:
    // Fills srv/format/width/height/mipLevels of `entry` from the resolved
    // bytes (loose file or archive memory). Static + device-parametrized so the
    // async path can run it on a worker (DirectXTex + the free-threaded device
    // only; touches no repository state).
    static void DecodeEntry(ID3D11Device* device, Entry& entry, const ResourceBytes& found);
    void LoadEntry(Entry& entry, const ResourceBytes& found) { DecodeEntry(m_device, entry, found); }

    // One in-flight async decode (shared between the worker and its UI
    // completion). Holds its own resolved bytes so the worker is self-contained.
    struct PendingTex;
    void PublishPrefetched(const std::shared_ptr<PendingTex>& job); // UI thread

    ID3D11Device* m_device = nullptr;
    std::uint64_t m_deviceGeneration { 0 };
    std::uint64_t m_publicationGeneration { 1 };
    ResourceResolver* m_resolver = nullptr;
    ResourceManager* m_manager = nullptr;
    ResourceManager::Token m_token {}; // stable (never re-bumped) so texture completions always run
    std::unordered_map<std::string, Entry> m_bySource; // key = ResourceBytes::sourceKey
};

} // namespace nsk
