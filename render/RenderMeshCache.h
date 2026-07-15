// RenderMeshCache.h - per-view GPU geometry buffer cache.
//
// Split out of the old monolithic D3D11Renderer (which owned one cache per
// viewport alongside a full duplicate of every shader/state/IBL resource).
// The immutable device-level resources now live once in RenderDevice; only
// this geometry cache stays per-view, because its keys are borrowed
// NifGeometry pointers whose lifetime is tied to that view's currently
// loaded NifDocument (a rebuild/clear of one view must not evict another
// view's still-valid buffers). RenderDevice fills these buffers on demand;
// the owning view clears the cache when its document changes.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_map>

namespace nsk
{

struct NifGeometry;

// Triangle-list vertex/index buffers for one NifGeometry.
struct GpuMesh
{
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
    UINT indexCount = 0;
};

// Per-vertex normal/tangent line segments (model space), normal segments
// first then tangent segments, so either overlay can be drawn without the
// other. See RenderDevice::GetOrCreateLineMesh.
struct GpuLineMesh
{
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    UINT normalVertexCount = 0;
    UINT tangentVertexStart = 0;
    UINT tangentVertexCount = 0;
};

// One view's geometry buffers, keyed by borrowed NifGeometry pointer. Clear()
// on the owning view's RebuildScene (before the old document's geometries are
// freed - the map only holds the pointers as keys, never dereferences them).
struct RenderMeshCache
{
    std::unordered_map<const NifGeometry*, GpuMesh> meshes;
    std::unordered_map<const NifGeometry*, GpuLineMesh> lines;

    void Clear()
    {
        meshes.clear();
        lines.clear();
    }
};

} // namespace nsk
