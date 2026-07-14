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

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nsk
{

class ResourceResolver;
struct ResourceBytes;

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
    };

    // Set from NifViewport::OnAttached; idempotent (single shared device).
    void SetDevice(ID3D11Device* device) { m_device = device; }

    // Resolve relativePath (against nifDirectory) and return the pooled
    // entry for the resolved source, loading/decoding/uploading it only if
    // this is the first time that source is seen. Returns nullptr when the
    // path does not resolve at all; an entry with a null srv means the
    // source resolved but failed to decode. Returned pointers stay valid
    // until Clear() (unordered_map nodes are stable).
    Entry* GetOrLoad(const std::string& relativePath, const std::wstring& nifDirectory);

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
    void Prefetch(const std::vector<std::string>& relativePaths, const std::wstring& nifDirectory);

    void Clear() { m_bySource.clear(); }

private:
    // Fills srv/format/width/height/mipLevels of `entry` from the resolved
    // bytes (loose file or archive memory).
    void LoadEntry(Entry& entry, const ResourceBytes& found);

    ID3D11Device* m_device = nullptr;
    ResourceResolver* m_resolver = nullptr;
    std::unordered_map<std::string, Entry> m_bySource; // key = ResourceBytes::sourceKey
};

} // namespace nsk
