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

namespace nsk
{

class ResourceResolver;

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

    void Clear() { m_bySource.clear(); }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadFromDisk(const std::wstring& fullPath);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadFromMemory(std::span<const std::uint8_t> data);

    ID3D11Device* m_device = nullptr;
    ResourceResolver* m_resolver = nullptr;
    std::unordered_map<std::string, Entry> m_bySource; // key = ResourceBytes::sourceKey
};

} // namespace nsk
