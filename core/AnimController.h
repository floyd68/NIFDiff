// AnimController.h - Qt-free port of the animation-playback math from
// NifSkope's src/gl/glcontroller.cpp: Controller::ctrlTime's extrapolation,
// Controller::timeIndex's cached key search, and the per-KeyType value
// interpolation (linear / quadratic Hermite / constant; TBC falls back to
// linear exactly like NifSkope's default branch). Pure sampling functions -
// no scene state; the caller owns the cached last-key indices that make
// sequential playback O(1) per frame.
#pragma once

#include "NifDocument.h"
#include "SceneBuilder.h" // RenderMesh (AnimPlayer::update rewrites worldTransforms)

namespace nsk::anim
{

// Map a raw scene time onto the controller's [start, stop] window, applying
// frequency/phase and the cycle-type extrapolation (Loop wraps, Reverse
// ping-pongs, Clamp holds the ends). Port of Controller::ctrlTime.
float ctrlTime(float time, float start, float stop, float frequency, float phase,
               NifTimeController::Cycle cycle);

// Sample one key group at `time` (already ctrlTime'd). Returns false when the
// group is empty (leave the output untouched). `lastIndex` caches the key
// search across calls - pass the same int for the same channel every frame.
bool sampleKeys(const NifKeyGroup<float>& group, float time, float& out, int& lastIndex);
bool sampleKeys(const NifKeyGroup<Vector3>& group, float time, Vector3& out, int& lastIndex);

// Rotation channel of a NifTransformData: quaternion keys (slerp, short path)
// or - for XyzRotation - the three per-axis Euler channels composed as
// Rz * Ry * Rx (glcontroller.cpp's Matrix::euler composition). `lastIndex`
// needs 3 ints (per-axis caches; quat keys use just the first).
bool sampleRotation(const NifTransformData& data, float time, Matrix& out, int lastIndex[3]);

// Drives one document's transform animations: binds the parsed controller
// blocks to scene nodes (both standalone always-playing NiTransformControllers
// and NiControllerManager sequences, mirroring NifSkope's
// ControllerManager::setSequence), then per frame samples every active channel
// into node-local transforms, recomposes the world hierarchy and rewrites each
// RenderMesh::worldTransform. Rigid transforms only: skinned meshes
// (ownedGeometry set) bake their pose into vertices and are left untouched.
class AnimPlayer
{
public:
    // `doc` must outlive this player (pointers into its parsed blocks are
    // kept). Gathers sequences + standalone controllers; selects no sequence.
    void bind(const NifDocument& doc);

    bool hasAnimations() const { return !m_sequences.empty() || !m_standalone.empty(); }
    // Named sequences ("Idle", "Open", ...) found on NiControllerManagers.
    std::size_t sequenceCount() const { return m_sequences.size(); }
    const std::string& sequenceName(std::size_t i) const { return m_sequences[i].name; }
    // index < 0 deactivates sequences (standalone controllers keep playing).
    void selectSequence(int index);
    int selectedSequence() const { return m_activeSequence; }

    // Playable time range of the current selection (sequence range, or the
    // union of the standalone controllers').
    float timeMin() const;
    float timeMax() const;

    // Sample every active channel at `time` (seconds), recompute node worlds
    // and rewrite the rigid meshes' worldTransform. Meshes must come from
    // SceneBuilder::build on the same document (sourceNodeIndex mapping).
    void update(float time, std::vector<RenderMesh>& meshes);

private:
    // One animated node channel: the interpolator (default pose) + optional
    // key data, with the controller/sequence timing that scopes its playback.
    struct Channel
    {
        int nodeIndex = -1;
        const NifTransformInterpolator* interp = nullptr;
        const NifTransformData* data = nullptr; // null = pose-only
        float start = 0.0f, stop = 0.0f, frequency = 1.0f, phase = 0.0f;
        NifTimeController::Cycle cycle = NifTimeController::Cycle::Clamp;
        int lastRot[3] = { 0, 0, 0 };
        int lastTrans = 0;
        int lastScale = 0;
    };
    struct Sequence
    {
        std::string name;
        float start = 0.0f, stop = 0.0f;
        std::vector<Channel> channels;
    };

    void evalChannel(Channel& ch, float time);

    const NifDocument* m_doc = nullptr;
    std::vector<Channel> m_standalone;  // active NiTransformControllers outside any manager
    std::vector<Sequence> m_sequences;
    int m_activeSequence = -1;
    std::vector<Transform> m_animLocals;    // per-node scratch (bind local unless animated)
    std::vector<Transform> m_worldCache;    // per-node scratch
    std::vector<std::int8_t> m_worldState;  // per-node scratch
};

} // namespace nsk::anim
