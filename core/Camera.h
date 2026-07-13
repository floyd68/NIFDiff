// Camera.h - Qt-free port of the orbit-camera math from src/gl/glview.cpp's
// mouse/wheel handlers (View menu "front/back/left/...", drag-to-rotate,
// wheel-to-zoom, middle-drag-to-pan). GLView mixed this logic directly into
// QGLWidget event handlers operating on Qt types (QPoint/QPointF); Camera
// is the same math against plain floats/NifTypes so it can be driven from
// FD2D::Wnd::OnInputEvent (see NifViewport.h) without any Qt dependency.
#pragma once

#include "NifTypes.h"
#include <algorithm>
#include <cmath>

namespace nsk
{

class Camera
{
public:
    // Default orbit for a fresh/reset view: eye raised above the target,
    // looking gently down (~20 degrees) - the typical 3D-editor resting
    // view. Pitch sign: eyePosition() = target - forward*distance with
    // forward.y = sin(pitch), so a NEGATIVE pitch puts the eye ABOVE the
    // target (a positive one would leave the camera underground, staring at
    // the grid's underside).
    static constexpr float kDefaultYaw = 0.0f;
    static constexpr float kDefaultPitch = -0.35f;

    void orbit(float deltaYawRad, float deltaPitchRad)
    {
        m_yaw += deltaYawRad;
        m_pitch = std::clamp(m_pitch + deltaPitchRad, -1.55f, 1.55f); // avoid gimbal flip at the poles
    }

    // Absolute orbit angles (same clamping as orbit()).
    void setOrbit(float yawRad, float pitchRad)
    {
        m_yaw = yawRad;
        m_pitch = std::clamp(pitchRad, -1.55f, 1.55f);
    }

    void pan(float dxWorld, float dyWorld)
    {
        Vector3 right, up;
        basisVectors(right, up);
        m_target += right * dxWorld + up * dyWorld;
    }

    void dolly(float deltaDistance)
    {
        m_distance = std::max(0.01f, m_distance + deltaDistance);
    }

    void setDistance(float d) { m_distance = std::max(0.01f, d); }
    float distance() const { return m_distance; }

    void setTarget(const Vector3& t) { m_target = t; }
    const Vector3& target() const { return m_target; }

    void frame(const Vector3& center, float radius)
    {
        m_target = center;
        m_distance = std::max(radius * 2.2f, 0.01f);
    }

    // Preset orientations, mirroring GLView's View > Front/Back/Left/Right/Top/Bottom.
    void setPreset(int presetIndex)
    {
        static const float kPresets[6][2] = {
            {0.0f, 0.0f},               // Front
            {NSK_PI, 0.0f},             // Back
            {NSK_PI * 0.5f, 0.0f},      // Left
            {-NSK_PI * 0.5f, 0.0f},     // Right
            {0.0f, NSK_PI * 0.5f - 0.001f}, // Top
            {0.0f, -(NSK_PI * 0.5f - 0.001f)}, // Bottom
        };
        int i = std::clamp(presetIndex, 0, 5);
        m_yaw = kPresets[i][0];
        m_pitch = kPresets[i][1];
    }

    Vector3 eyePosition() const
    {
        Vector3 dir = forwardVector();
        return m_target - dir * m_distance;
    }

    Vector3 forwardVector() const
    {
        float cp = std::cos(m_pitch);
        return Vector3(std::sin(m_yaw) * cp, std::sin(m_pitch), std::cos(m_yaw) * cp);
    }

    // Left-handed look-at (D3DXMatrixLookAtLH convention: view-space forward
    // is +Z), matching the projectionMatrix() below and the HLSL shaders'
    // mul(float4(pos,1), worldViewProj) column-vector usage after
    // Matrix4::toColumnMajor() upload.
    Matrix4 viewMatrix() const
    {
        Vector3 eye = eyePosition();
        Vector3 forward = forwardVector();
        forward.normalize();
        Vector3 worldUp(0.0f, 1.0f, 0.0f);
        Vector3 right = Vector3::crossproduct(worldUp, forward);
        if (right.squaredLength() < 1e-8f)
            right = Vector3(1.0f, 0.0f, 0.0f);
        right.normalize();
        Vector3 up = Vector3::crossproduct(forward, right);
        up.normalize();

        Matrix4 m;
        m(0, 0) = right[0];   m(0, 1) = right[1];   m(0, 2) = right[2];   m(0, 3) = -Vector3::dotproduct(right, eye);
        m(1, 0) = up[0];      m(1, 1) = up[1];      m(1, 2) = up[2];      m(1, 3) = -Vector3::dotproduct(up, eye);
        m(2, 0) = forward[0]; m(2, 1) = forward[1]; m(2, 2) = forward[2]; m(2, 3) = -Vector3::dotproduct(forward, eye);
        m(3, 0) = 0.0f; m(3, 1) = 0.0f; m(3, 2) = 0.0f; m(3, 3) = 1.0f;
        return m;
    }

    static Matrix4 projectionMatrix(float fovYRadians, float aspect, float nearZ, float farZ)
    {
        Matrix4 m;
        float f = 1.0f / std::tan(fovYRadians * 0.5f);
        std::memset(&m.m[0][0], 0, sizeof(m.m));
        m(0, 0) = f / aspect;
        m(1, 1) = f;
        m(2, 2) = farZ / (farZ - nearZ);
        m(2, 3) = -(farZ * nearZ) / (farZ - nearZ);
        m(3, 2) = 1.0f;
        return m;
    }

private:
    // Same handedness as viewMatrix() (right = worldUp x forward, up =
    // forward x right), so pan()'s +x really is screen-right. The previous
    // forward x worldUp produced the NEGATED screen-right (up came out
    // identical), which made horizontal pan move against the drag.
    void basisVectors(Vector3& right, Vector3& up) const
    {
        Vector3 forward = forwardVector();
        Vector3 worldUp(0.0f, 1.0f, 0.0f);
        right = Vector3::crossproduct(worldUp, forward);
        if (right.squaredLength() < 1e-8f)
            right = Vector3(1.0f, 0.0f, 0.0f);
        right.normalize();
        up = Vector3::crossproduct(forward, right);
        up.normalize();
    }

    Vector3 m_target;
    float m_distance = 200.0f;
    float m_yaw = kDefaultYaw;
    float m_pitch = kDefaultPitch;
};

} // namespace nsk
