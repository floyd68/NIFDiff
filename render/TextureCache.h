// TextureCache.h - per-viewport memo in front of the shared
// TextureRepository. Resolution depends on the viewport's NIF directory
// (search order: overrides -> nif dir -> game data -> archives), so each
// viewport memoizes relative path -> pooled Entry for ITS resolution
// context; the heavy work (file/archive read, DDS decode, SRV upload,
// complex-material probes) lives in the repository, done once per unique
// resolved source across all panes.
#pragma once

#include "TextureRepository.h"

#include <string>
#include <unordered_map>

namespace nsk
{

class TextureCache
{
public:
    explicit TextureCache(TextureRepository* repository)
        : m_repository(repository)
    {
    }

    // Changing the NIF directory changes what relative paths resolve to, so
    // the memo is dropped (the repository keeps the loaded textures - a
    // re-resolve that lands on the same source is a pool hit).
    void SetNifDirectory(std::wstring nifDir)
    {
        if (nifDir == m_nifDirectory)
            return;
        m_nifDirectory = std::move(nifDir);
        m_memo.clear();
    }

    // relativePath uses forward slashes (see NifDocument::normalizeSlashes).
    // Returns a pooled or freshly loaded SRV, or nullptr when the path does
    // not resolve / fails to decode - the renderer decides how to present a
    // missing texture per slot (D3D11Renderer's resolve lambda). Failures
    // are memoized too, so an unresolvable path doesn't re-run the resolver
    // chain every frame.
    ID3D11ShaderResourceView* GetOrLoad(const std::string& relativePath)
    {
        TextureRepository::Entry* e = Lookup(relativePath);
        return e != nullptr ? e->srv.Get() : nullptr;
    }

    // CPU-side ENB/CS "complex material" probes (see TextureRepository::
    // EnsureCmProbe for the detection rules). Both verdicts come from one
    // probe, computed at most once per unique texture process-wide.
    bool HasComplexMaterialAlpha(const std::string& relativePath)
    {
        TextureRepository::Entry* e = Lookup(relativePath);
        if (e == nullptr)
            return false;
        m_repository->EnsureCmProbe(*e);
        return e->cmAlpha;
    }

    // Parallel-prefetch every path under this cache's resolution context
    // (see TextureRepository::Prefetch); later GetOrLoad calls become pool
    // hits. The per-path memo still forms lazily on first use.
    void Prefetch(const std::vector<std::string>& relativePaths)
    {
        if (m_repository != nullptr)
            m_repository->Prefetch(relativePaths, m_nifDirectory);
    }

    bool HasComplexMaterialHeight(const std::string& relativePath)
    {
        TextureRepository::Entry* e = Lookup(relativePath);
        if (e == nullptr)
            return false;
        m_repository->EnsureCmProbe(*e);
        return e->cmHeight;
    }

    void Clear() { m_memo.clear(); }

private:
    TextureRepository::Entry* Lookup(const std::string& relativePath)
    {
        if (relativePath.empty() || m_repository == nullptr)
            return nullptr;
        auto it = m_memo.find(relativePath);
        if (it != m_memo.end())
            return it->second; // may be nullptr: memoized "does not resolve"
        TextureRepository::Entry* e = m_repository->GetOrLoad(relativePath, m_nifDirectory);
        m_memo.emplace(relativePath, e);
        return e;
    }

    TextureRepository* m_repository = nullptr;
    std::wstring m_nifDirectory;
    // Entry pointers stay valid until TextureRepository::Clear (node-based
    // map); the view-level invalidation clears repository and memos together.
    std::unordered_map<std::string, TextureRepository::Entry*> m_memo;
};

} // namespace nsk
