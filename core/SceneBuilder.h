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
// (a) resolving world transforms and flattening the shape hierarchy into a
// single render list, PLUS (b) static (bind-pose) matrix-palette skinning
// for NiSkinPartition-backed shapes - see applySkinning() in the .cpp,
// which mirrors NifSkope's BSShape::transformShapes() (bsshape.cpp) exactly
// and was verified against a live NifSkope render of real Skyrim SE content
// with Do Skinning toggled both ways.
//
// Explicitly still dropped vs. the original scenegraph: bone *animation*
// (this only ever shows the skeleton's bind/rest pose), morph targets,
// particle systems (glparticles.cpp), and all other NiTimeController
// playback (controllers.cpp) - none of these affect a static bind-pose mesh
// comparison, which is the feature NIF_DIFF_VIEWER.md actually asks for.
#pragma once

#include "NifDocument.h"
#include <memory>
#include <string>
#include <vector>

namespace nsk
{

class ResourceResolver;

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
    // Index of this shape's node in the owning NifDocument::nodes() array, so
    // an animation runtime can recompute worldTransform per frame from the
    // (animated) node hierarchy. -1 only if a mesh is constructed outside
    // build(). Note: skinned meshes (ownedGeometry set) bake their pose into
    // the vertices and ignore the node world - a rigid animation pass must
    // skip those (bind pose until Phase 4 per-frame re-skinning).
    int sourceNodeIndex = -1;
    const NifGeometry* geometry = nullptr; // borrowed pointer into the owning NifDocument, or into ownedGeometry below
    // Set only for skinned shapes (see applySkinning()): geometry with
    // world-space-skinned positions can't be shared with the NifDocument's
    // own (reference-pose) storage, so it is owned here instead. geometry
    // points into this when set; null otherwise (the common, rigid-shape case).
    std::shared_ptr<NifGeometry> ownedGeometry;
    NifMaterial material;                  // copied (cheap: a few floats + 2 short paths)
};

class SceneBuilder
{
public:
    // doc must outlive the returned RenderMesh list (geometry pointers are
    // borrowed references into doc's internal storage). includeHidden keeps
    // NiAVObject-hidden subtrees (furniture marker rigs, editor markers,
    // bounds placeholders) in the scene - the engine never draws them, so
    // they are culled by default; the UI's "Hidden" display toggle opts
    // back in, NifSkope's Show Hidden equivalent.
    //
    // resolver, if given, is used to look up a reference skeleton NifDocument
    // (ResourceResolver::GetSkeletonDocument) for FaceGen-style shapes whose
    // own in-file skin bones carry no real position - see applySkinning() in
    // the .cpp. Left null, those shapes fall back to their in-file bone
    // resolution as before (unaffected for ordinary skinned content, which
    // never consults the skeleton doc regardless).
    [[nodiscard]] static std::vector<RenderMesh> build(const NifDocument& doc, bool includeHidden = false,
        const ResourceResolver* resolver = nullptr);

    // The fixed Z-up -> Y-up rotation baked into every mesh's worldTransform
    // (see axisCorrectionZupToYup in the .cpp). Exposed so the animation
    // runtime (AnimPlayer) applies the same convention when it recomputes
    // world transforms per frame.
    [[nodiscard]] static Matrix4 axisCorrection();
};

} // namespace nsk
