// AnimController.h - Qt-free port of the animation-playback math from
// NifSkope's src/gl/glcontroller.cpp: Controller::ctrlTime's extrapolation,
// Controller::timeIndex's cached key search, and the per-KeyType value
// interpolation (linear / quadratic Hermite / constant; TBC falls back to
// linear exactly like NifSkope's default branch). Pure sampling functions -
// no scene state; the caller owns the cached last-key indices that make
// sequential playback O(1) per frame.
#pragma once

#include "NifDocument.h"

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

} // namespace nsk::anim
