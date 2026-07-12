// ValidateNif.cpp - Phase 6 validation helper (phase6_validation).
//
// A tiny console-mode companion tool (separate from the WIN32 NifLiteViewer
// executable) that loads a .nif through NifDocument + SceneBuilder and
// prints a structural summary: block/node counts, per-shape vertex/index
// counts, and material/texture-set resolution. This gives a fast,
// automatable first check ("did the parser understand this file at all?")
// before doing the actual side-by-side visual comparison in NifLiteViewer
// itself against the sample files in test_assets/.
#include "../core/NifDocument.h"
#include "../core/SceneBuilder.h"

#include <cstdio>
#include <string>

using namespace nsk;

namespace
{
    void PrintDocument(const NifDocument& doc, const wchar_t* path)
    {
        std::printf("== %ls ==\n", path);
        std::printf("  version       : %s (bsVersion=%u)\n", doc.versionString().c_str(), doc.bsVersion());
        std::printf("  blocks        : %d\n", doc.blockCount());
        std::printf("  scene nodes   : %zu\n", doc.nodes().size());
        std::printf("  root nodes    : %zu\n", doc.roots().size());
        std::printf("  geometries    : %zu\n", doc.geometries().size());
        std::printf("  materials     : %zu\n", doc.materials().size());
        std::printf("  texture sets  : %zu\n", doc.textureSets().size());

        auto meshes = SceneBuilder::build(doc);
        std::printf("  render meshes : %zu\n", meshes.size());
        std::size_t totalVerts = 0, totalTris = 0;
        for (const auto& m : meshes)
        {
            if (m.geometry)
            {
                totalVerts += m.geometry->positions.size();
                totalTris += m.geometry->triangles.size();
            }
        }
        std::printf("  total verts   : %zu\n", totalVerts);
        std::printf("  total tris    : %zu\n", totalTris);

        for (std::size_t i = 0; i < meshes.size(); ++i)
        {
            const auto& m = meshes[i];
            const std::size_t nv = m.geometry ? m.geometry->positions.size() : 0;
            const std::size_t nt = m.geometry ? m.geometry->triangles.size() : 0;
            std::printf("    [%zu] %-32s verts=%-6zu tris=%-6zu diffuse=%s\n",
                i, m.nodeName.c_str(), nv, nt,
                m.material.diffuseTexture.empty() ? "(none)" : m.material.diffuseTexture.c_str());
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("Usage: ValidateNif <file1.nif> [file2.nif ...]\n");
        return 1;
    }

    int failures = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::string narrow = argv[i];
        std::wstring path(narrow.begin(), narrow.end());

        NifDocument doc;
        std::string error;
        if (!doc.loadFromFile(path, &error))
        {
            std::printf("== %s ==\n  FAILED TO LOAD: %s\n\n", argv[i], error.c_str());
            ++failures;
            continue;
        }
        PrintDocument(doc, path.c_str());
    }

    if (failures > 0)
        std::printf("%d of %d file(s) failed to load.\n", failures, argc - 1);
    else
        std::printf("All %d file(s) loaded successfully.\n", argc - 1);

    return failures > 0 ? 1 : 0;
}
