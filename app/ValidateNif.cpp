// ValidateNif.cpp - Phase 6 validation helper (phase6_validation).
//
// A tiny console-mode companion tool (separate from the WIN32 NIFDiff
// executable) that loads a .nif through NifDocument + SceneBuilder and
// prints a structural summary: block/node counts, per-shape vertex/index
// counts, and material/texture-set resolution. This gives a fast,
// automatable first check ("did the parser understand this file at all?")
// before doing the actual side-by-side visual comparison in NIFDiff itself
// against the sample files in test_assets/.
#include "../core/NifDocument.h"
#include "../core/SceneBuilder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

using namespace nsk;

namespace
{
    // UTF-8 narrowing for display only - paths stay wide throughout (see
    // wmain below for why the entry point is wide in the first place).
    std::string toUtf8(std::wstring_view w)
    {
        if (w.empty())
            return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
        return s;
    }

    void PrintDocument(const NifDocument& doc, std::wstring_view path)
    {
        std::cout << std::format("== {} ==\n", toUtf8(path));
        std::cout << std::format("  version       : {} (bsVersion={})\n", doc.versionString(), doc.bsVersion());
        std::cout << std::format("  blocks        : {}\n", doc.blockCount());
        std::cout << std::format("  scene nodes   : {}\n", doc.nodes().size());
        std::cout << std::format("  root nodes    : {}\n", doc.roots().size());
        std::cout << std::format("  geometries    : {}\n", doc.geometries().size());
        std::cout << std::format("  materials     : {}\n", doc.materials().size());
        std::cout << std::format("  texture sets  : {}\n", doc.textureSets().size());
        for (const auto& [blockIdx, ti] : doc.transformInterpolators())
        {
            std::cout << std::format(
                "  anim interp   : block {} dataRef={} poseValid(t/r/s)={}/{}/{}\n",
                blockIdx, ti.dataRef,
                ti.translationValid(), ti.rotationValid(), ti.scaleValid());
        }
        // Animation keyframe channels (NiTransformData): per-block key counts,
        // so a parsed animated static can be sanity-checked against NifSkope.
        for (const auto& [blockIdx, td] : doc.transformData())
        {
            std::cout << std::format(
                "  anim data     : block {} rotType={} quatKeys={} xyzKeys={}/{}/{} transKeys={}(type {}) scaleKeys={}\n",
                blockIdx, static_cast<unsigned>(td.rotationType), td.quatKeys.size(),
                td.xyzRotations[0].keys.size(), td.xyzRotations[1].keys.size(), td.xyzRotations[2].keys.size(),
                td.translations.keys.size(), static_cast<unsigned>(td.translations.keyType),
                td.scales.keys.size());
        }

        auto meshes = SceneBuilder::build(doc);
        std::cout << std::format("  render meshes : {}\n", meshes.size());
        std::size_t totalVerts = 0, totalTris = 0;
        for (const auto& m : meshes)
        {
            if (m.geometry)
            {
                totalVerts += m.geometry->positions.size();
                totalTris += m.geometry->triangles.size();
            }
        }
        std::cout << std::format("  total verts   : {}\n", totalVerts);
        std::cout << std::format("  total tris    : {}\n", totalTris);

        for (std::size_t i = 0; i < meshes.size(); ++i)
        {
            const auto& m = meshes[i];
            const std::size_t nv = m.geometry ? m.geometry->positions.size() : 0;
            const std::size_t nt = m.geometry ? m.geometry->triangles.size() : 0;
            std::cout << std::format("    [{}] {:<32} verts={:<6} tris={:<6} diffuse={} skinned={}\n",
                i, m.nodeName, nv, nt,
                m.material.diffuseTexture.empty() ? "(none)" : m.material.diffuseTexture,
                m.ownedGeometry ? "yes" : "no");

            if (m.geometry && nv > 0)
            {
                Vector3 mn = m.worldTransform * m.geometry->positions[0];
                Vector3 mx = mn;
                for (std::size_t v = 0; v < nv; ++v)
                {
                    Vector3 p = m.worldTransform * m.geometry->positions[v];
                    for (int k = 0; k < 3; ++k)
                    {
                        mn[k] = std::min(mn[k], p[k]);
                        mx[k] = std::max(mx[k], p[k]);
                    }
                }
                std::cout << std::format("        world bbox min=({:.2f},{:.2f},{:.2f}) max=({:.2f},{:.2f},{:.2f}) size=({:.2f},{:.2f},{:.2f})\n",
                    mn[0], mn[1], mn[2],
                    mx[0], mx[1], mx[2],
                    mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
            }
        }
        std::cout << '\n';
    }
}

// Wide entry point: the previous narrow main() widened argv bytes 1:1
// (std::wstring(narrow.begin(), narrow.end())), which silently corrupts any
// non-ASCII path (e.g. Korean mod folder names). wmain receives the real
// UTF-16 command line, so paths round-trip losslessly into loadFromFile.
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8); // std::cout below emits UTF-8 bytes

    if (argc < 2)
    {
        std::cout << "Usage: ValidateNif <file1.nif> [file2.nif ...]\n";
        return 1;
    }

    int failures = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring path = argv[i];

        NifDocument doc;
        std::string error;
        if (!doc.loadFromFile(path, &error))
        {
            std::cout << std::format("== {} ==\n  FAILED TO LOAD: {}\n\n", toUtf8(path), error);
            ++failures;
            continue;
        }
        PrintDocument(doc, path);
    }

    if (failures > 0)
        std::cout << std::format("{} of {} file(s) failed to load.\n", failures, argc - 1);
    else
        std::cout << std::format("All {} file(s) loaded successfully.\n", argc - 1);

    return failures > 0 ? 1 : 0;
}
