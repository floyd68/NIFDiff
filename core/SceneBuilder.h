// SceneBuilder.h - Qt-free stand-in for src/gl/glscene.h + glnode.h's role of
// walking the NIF block graph into a renderable scene.
//
// Scope note: the original Scene/Node/Mesh/BSShape classes are QObject-based
// wrappers around a NifModel that (a) resolve the live QModelIndex tree into
// a parent/child node hierarchy, (b) evaluate .prog shader condition
// expressions per NiProperty, (c) drive skinning/morphing/particle
// controllers every frame, and (d) hand off per-node draw calls to
// Renderer. Because this lite viewer's target is a static-pose comparison
// view (no animation/particle playback - see NIF_DIFF_VIEWER.md), and
// because NifDocument already parses geometry + shader/alpha property refs
// directly (see NifDocument.h), the only scenegraph responsibility left is
// (a): resolving world transforms and flattening the shape hierarchy into a
// single render list. That is what SceneBuilder does, in about a hundred
// lines instead of glscene.cpp+glnode.cpp's several thousand.
//
// Explicitly dropped vs. the original scenegraph (documented here rather
// than silently, per the plan's Phase 3 scope): skinning/bone deformation,
// morph targets, particle systems (glparticles.cpp), and all NiTimeController
// playback (controllers.cpp) - none of these affect a static bind-pose mesh
// comparison, which is the feature NIF_DIFF_VIEWER.md actually asks for.
#pragma once

#include "NifDocument.h"
#include <string>
#include <vector>

namespace nsk
{

// Default material used for shapes with no resolved BSLightingShaderProperty/
// NiMaterialProperty (e.g. collision proxies, or a block type this parser
// does not understand) so the renderer always has something to bind.
inline NifMaterial defaultMaterial()
{
    NifMaterial m;
    m.specularColor = Color3(0.2f, 0.2f, 0.2f);
    return m;
}

struct RenderMesh
{
    std::string nodeName;
    Matrix4 worldTransform;
    const NifGeometry* geometry = nullptr; // borrowed pointer into the owning NifDocument
    NifMaterial material;                  // copied (cheap: a few floats + 2 short paths)
};

class SceneBuilder
{
public:
    // doc must outlive the returned RenderMesh list (geometry pointers are
    // borrowed references into doc's internal storage).
    [[nodiscard]] static std::vector<RenderMesh> build(const NifDocument& doc);
};

} // namespace nsk
