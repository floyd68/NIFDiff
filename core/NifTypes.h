// NifTypes.h - Qt-free port of src/data/niftypes.h
//
// Scope note (Phase 2 of the FD2D/D3D11 lite-viewer plan): this is a faithful,
// header-only rewrite of the math/geometry value types NifSkope uses
// (Vector2/3/4, Quat, Matrix, Matrix4, Transform, Triangle, Color3/4,
// BSVertexDesc) using only the C++ standard library. All QColor/QDataStream/
// QDebug/QString dependencies have been removed; QDataStream operators are
// replaced by NifStream (see NifStream.h) and QColor conversion helpers are
// dropped since the lite viewer has no Qt-based color picker UI.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include <numbers>

namespace nsk
{

inline constexpr float NSK_PI = std::numbers::pi_v<float>;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

inline float Clamp01(float a)
{
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

class Vector2
{
public:
    Vector2() { xy[0] = xy[1] = 0.0f; }
    Vector2(float x, float y) { xy[0] = x; xy[1] = y; }

    Vector2& operator+=(const Vector2& v) { xy[0] += v.xy[0]; xy[1] += v.xy[1]; return *this; }
    Vector2 operator+(const Vector2& v) const { Vector2 w(*this); return w += v; }
    Vector2& operator-=(const Vector2& v) { xy[0] -= v.xy[0]; xy[1] -= v.xy[1]; return *this; }
    Vector2 operator-(const Vector2& v) const { Vector2 w(*this); return w -= v; }
    Vector2 operator-() const { return Vector2() - *this; }
    Vector2& operator*=(float s) { xy[0] *= s; xy[1] *= s; return *this; }
    Vector2 operator*(float s) const { Vector2 w(*this); return w *= s; }
    Vector2& operator/=(float s) { xy[0] /= s; xy[1] /= s; return *this; }
    Vector2 operator/(float s) const { Vector2 w(*this); return w /= s; }
    bool operator==(const Vector2& v) const = default; // memberwise float ==, C++20 defaulted comparison

    float& operator[](unsigned i) { return xy[i]; }
    const float& operator[](unsigned i) const { return xy[i]; }
    const float* data() const { return xy; }

    float xy[2];
};

class HalfVector2 : public Vector2
{
public:
    HalfVector2() : Vector2() {}
    HalfVector2(float x, float y) : Vector2(x, y) {}
    HalfVector2(Vector2 v) : Vector2(v) {}
};

class Vector4;

class Vector3
{
public:
    Vector3() { xyz[0] = xyz[1] = xyz[2] = 0.0f; }
    Vector3(float x, float y, float z) { xyz[0] = x; xyz[1] = y; xyz[2] = z; }
    explicit Vector3(const Vector2& v2, float z = 0.0f) { xyz[0] = v2[0]; xyz[1] = v2[1]; xyz[2] = z; }
    explicit Vector3(const Vector4& v4);

    Vector3& operator+=(const Vector3& v) { xyz[0] += v.xyz[0]; xyz[1] += v.xyz[1]; xyz[2] += v.xyz[2]; return *this; }
    Vector3& operator-=(const Vector3& v) { xyz[0] -= v.xyz[0]; xyz[1] -= v.xyz[1]; xyz[2] -= v.xyz[2]; return *this; }
    Vector3& operator*=(float s) { xyz[0] *= s; xyz[1] *= s; xyz[2] *= s; return *this; }
    Vector3& operator/=(float s) { xyz[0] /= s; xyz[1] /= s; xyz[2] /= s; return *this; }
    Vector3 operator+(const Vector3& v) const { Vector3 w(*this); return w += v; }
    Vector3 operator-(const Vector3& v) const { Vector3 w(*this); return w -= v; }
    Vector3 operator-() const { return Vector3() - *this; }
    Vector3 operator*(float s) const { Vector3 v(*this); return v *= s; }
    Vector3 operator/(float s) const { Vector3 v(*this); return v /= s; }
    bool operator==(const Vector3& v) const = default; // memberwise float ==, C++20 defaulted comparison

    float& operator[](unsigned i) { return xyz[i]; }
    const float& operator[](unsigned i) const { return xyz[i]; }

    float length() const { return std::sqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]); }
    float squaredLength() const { return xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]; }

    Vector3& normalize()
    {
        float m = length();
        m = (m > 0.0f) ? 1.0f / m : 0.0f;
        xyz[0] *= m; xyz[1] *= m; xyz[2] *= m;
        return *this;
    }

    static float dotproduct(const Vector3& v1, const Vector3& v2)
    {
        return v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
    }
    static Vector3 crossproduct(const Vector3& a, const Vector3& b)
    {
        return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    }

    void boundMin(const Vector3& v) { for (int i = 0; i < 3; i++) if (v[i] < xyz[i]) xyz[i] = v[i]; }
    void boundMax(const Vector3& v) { for (int i = 0; i < 3; i++) if (v[i] > xyz[i]) xyz[i] = v[i]; }

    const float* data() const { return xyz; }

    float xyz[3];
};

class HalfVector3 : public Vector3
{
public:
    HalfVector3() : Vector3() {}
    HalfVector3(float x, float y, float z) : Vector3(x, y, z) {}
    HalfVector3(Vector3 v) : Vector3(v) {}
};

class ByteVector3 : public Vector3
{
public:
    ByteVector3() : Vector3() {}
    ByteVector3(float x, float y, float z) : Vector3(x, y, z) {}
    ByteVector3(Vector3 v) : Vector3(v) {}
};

class Vector4
{
public:
    Vector4() { xyzw[0] = xyzw[1] = xyzw[2] = xyzw[3] = 0.0f; }
    Vector4(float x, float y, float z, float w) { xyzw[0] = x; xyzw[1] = y; xyzw[2] = z; xyzw[3] = w; }
    explicit Vector4(const Vector3& v3, float w = 0.0f) { xyzw[0] = v3[0]; xyzw[1] = v3[1]; xyzw[2] = v3[2]; xyzw[3] = w; }

    Vector4& operator+=(const Vector4& v) { for (int i = 0; i < 4; i++) xyzw[i] += v.xyzw[i]; return *this; }
    Vector4& operator-=(const Vector4& v) { for (int i = 0; i < 4; i++) xyzw[i] -= v.xyzw[i]; return *this; }
    Vector4 operator+(const Vector4& v) const { Vector4 w(*this); return w += v; }
    Vector4 operator-(const Vector4& v) const { Vector4 w(*this); return w -= v; }
    Vector4 operator*(float s) const { Vector4 v(*this); for (int i = 0; i < 4; i++) v.xyzw[i] *= s; return v; }
    bool operator==(const Vector4& v) const = default; // memberwise float ==, C++20 defaulted comparison

    float& operator[](unsigned i) { return xyzw[i]; }
    const float& operator[](unsigned i) const { return xyzw[i]; }
    const float* data() const { return xyzw; }

    float xyzw[4];
};

inline Vector3::Vector3(const Vector4& v4) { xyz[0] = v4[0]; xyz[1] = v4[1]; xyz[2] = v4[2]; }

class Quat
{
public:
    Quat() { wxyz[0] = 1.0f; wxyz[1] = wxyz[2] = wxyz[3] = 0.0f; }
    Quat(float w, float x, float y, float z) { wxyz[0] = w; wxyz[1] = x; wxyz[2] = y; wxyz[3] = z; }

    float& operator[](unsigned i) { return wxyz[i]; }
    const float& operator[](unsigned i) const { return wxyz[i]; }

    void normalize()
    {
        float mag = wxyz[0] * wxyz[0] + wxyz[1] * wxyz[1] + wxyz[2] * wxyz[2] + wxyz[3] * wxyz[3];
        mag = std::sqrt(mag);
        if (mag > 0.0f)
            for (int i = 0; i < 4; i++) wxyz[i] /= mag;
    }

    float wxyz[4];
};

// 3x3 rotation matrix (row-major, matches original NifSkope layout).
class Matrix
{
public:
    Matrix()
    {
        std::memset(m, 0, sizeof(m));
        m[0][0] = m[1][1] = m[2][2] = 1.0f;
    }

    Matrix operator*(const Matrix& m2) const
    {
        Matrix m3;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                m3.m[r][c] = m[r][0] * m2.m[0][c] + m[r][1] * m2.m[1][c] + m[r][2] * m2.m[2][c];
        return m3;
    }
    Vector3 operator*(const Vector3& v) const
    {
        return Vector3(
            m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]);
    }

    float& operator()(unsigned r, unsigned c) { return m[r][c]; }
    float operator()(unsigned r, unsigned c) const { return m[r][c]; }

    Matrix inverted() const
    {
        // Assumes an orthonormal rotation matrix: inverse == transpose.
        Matrix r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                r.m[i][j] = m[j][i];
        return r;
    }

    void fromQuat(const Quat& q)
    {
        float w = q[0], x = q[1], y = q[2], z = q[3];
        m[0][0] = 1 - 2 * y * y - 2 * z * z; m[0][1] = 2 * x * y - 2 * z * w;     m[0][2] = 2 * x * z + 2 * y * w;
        m[1][0] = 2 * x * y + 2 * z * w;     m[1][1] = 1 - 2 * x * x - 2 * z * z; m[1][2] = 2 * y * z - 2 * x * w;
        m[2][0] = 2 * x * z - 2 * y * w;     m[2][1] = 2 * y * z + 2 * x * w;     m[2][2] = 1 - 2 * x * x - 2 * y * y;
    }

    Quat toQuat() const
    {
        float trace = m[0][0] + m[1][1] + m[2][2];
        Quat q;
        if (trace > 0.0f)
        {
            float s = std::sqrt(trace + 1.0f) * 2.0f;
            q[0] = 0.25f * s;
            q[1] = (m[2][1] - m[1][2]) / s;
            q[2] = (m[0][2] - m[2][0]) / s;
            q[3] = (m[1][0] - m[0][1]) / s;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
            q[0] = (m[2][1] - m[1][2]) / s;
            q[1] = 0.25f * s;
            q[2] = (m[0][1] + m[1][0]) / s;
            q[3] = (m[0][2] + m[2][0]) / s;
        }
        else if (m[1][1] > m[2][2])
        {
            float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
            q[0] = (m[0][2] - m[2][0]) / s;
            q[1] = (m[0][1] + m[1][0]) / s;
            q[2] = 0.25f * s;
            q[3] = (m[1][2] + m[2][1]) / s;
        }
        else
        {
            float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
            q[0] = (m[1][0] - m[0][1]) / s;
            q[1] = (m[0][2] + m[2][0]) / s;
            q[2] = (m[1][2] + m[2][1]) / s;
            q[3] = 0.25f * s;
        }
        q.normalize();
        return q;
    }

    static Matrix euler(float x, float y, float z)
    {
        Matrix mx, my, mz;
        mx.m[1][1] = std::cos(x); mx.m[1][2] = -std::sin(x);
        mx.m[2][1] = std::sin(x); mx.m[2][2] = std::cos(x);
        my.m[0][0] = std::cos(y); my.m[0][2] = std::sin(y);
        my.m[2][0] = -std::sin(y); my.m[2][2] = std::cos(y);
        mz.m[0][0] = std::cos(z); mz.m[0][1] = -std::sin(z);
        mz.m[1][0] = std::sin(z); mz.m[1][1] = std::cos(z);
        return mz * my * mx;
    }

    const float* data() const { return &m[0][0]; }

    float m[3][3];
};

// 4x4 matrix (row-major).
class Matrix4
{
public:
    Matrix4()
    {
        std::memset(m, 0, sizeof(m));
        m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
    }

    Matrix4 operator*(const Matrix4& m2) const
    {
        Matrix4 m3;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                m3.m[r][c] = m[r][0] * m2.m[0][c] + m[r][1] * m2.m[1][c] + m[r][2] * m2.m[2][c] + m[r][3] * m2.m[3][c];
        return m3;
    }
    Vector3 operator*(const Vector3& v) const
    {
        return Vector3(
            m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2] + m[0][3],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2] + m[1][3],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] + m[2][3]);
    }

    float& operator()(unsigned r, unsigned c) { return m[r][c]; }
    float operator()(unsigned r, unsigned c) const { return m[r][c]; }

    void compose(const Vector3& trans, const Matrix& rot, const Vector3& scale)
    {
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                m[r][c] = rot(r, c) * scale[c];
        m[0][3] = trans[0]; m[1][3] = trans[1]; m[2][3] = trans[2];
        m[3][0] = m[3][1] = m[3][2] = 0.0f; m[3][3] = 1.0f;
    }

    void decompose(Vector3& trans, Matrix& rot, Vector3& scale) const
    {
        trans = Vector3(m[0][3], m[1][3], m[2][3]);
        for (int c = 0; c < 3; c++)
        {
            Vector3 col(m[0][c], m[1][c], m[2][c]);
            scale[c] = col.length();
        }
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                rot(r, c) = (scale[c] != 0.0f) ? m[r][c] / scale[c] : 0.0f;
    }

    // Column-major float array, ready for HLSL cbuffer upload (D3D11 expects
    // column-major by default when mapped straight into a float4x4 without a
    // transpose in the shader; we transpose here once on CPU instead of doing
    // it in every vertex shader invocation).
    void toColumnMajor(float out[16]) const
    {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                out[c * 4 + r] = m[r][c];
    }

    const float* data() const { return &m[0][0]; }

    float m[4][4];
};

class Transform
{
public:
    Transform() : scale(1.0f) {}

    Vector3 operator*(const Vector3& v) const { return rotation * v * scale + translation; }

    friend Transform operator*(const Transform& t1, const Transform& t2)
    {
        Transform t;
        t.rotation = t1.rotation * t2.rotation;
        t.translation = t1.rotation * t2.translation * t1.scale + t1.translation;
        t.scale = t1.scale * t2.scale;
        return t;
    }

    Matrix4 toMatrix4() const
    {
        Matrix4 m;
        m.compose(translation, rotation, Vector3(scale, scale, scale));
        return m;
    }

    Matrix rotation;
    Vector3 translation;
    float scale;
};

class Triangle
{
public:
    Triangle() { v[0] = v[1] = v[2] = 0; }
    Triangle(u16 a, u16 b, u16 c) { v[0] = a; v[1] = b; v[2] = c; }

    u16& operator[](unsigned i) { return v[i]; }
    const u16& operator[](unsigned i) const { return v[i]; }
    void set(u16 a, u16 b, u16 c) { v[0] = a; v[1] = b; v[2] = c; }
    u16 v1() const { return v[0]; }
    u16 v2() const { return v[1]; }
    u16 v3() const { return v[2]; }
    void flip() { std::swap(v[0], v[1]); }

    u16 v[3];
};

class Color3
{
public:
    Color3() { rgb[0] = rgb[1] = rgb[2] = 1.0f; }
    Color3(float r, float g, float b) { rgb[0] = r; rgb[1] = g; rgb[2] = b; }
    explicit Color3(const class Color4& c4);

    float& operator[](unsigned i) { return rgb[i]; }
    const float& operator[](unsigned i) const { return rgb[i]; }

    float red() const { return rgb[0]; }
    float green() const { return rgb[1]; }
    float blue() const { return rgb[2]; }

    const float* data() const { return rgb; }

    float rgb[3];
};

class Color4
{
public:
    Color4() { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.0f; }
    explicit Color4(const Color3& c, float alpha = 1.0f) { rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2]; rgba[3] = alpha; }
    Color4(float r, float g, float b, float a) { rgba[0] = r; rgba[1] = g; rgba[2] = b; rgba[3] = a; }

    float& operator[](unsigned i) { return rgba[i]; }
    const float& operator[](unsigned i) const { return rgba[i]; }

    float red() const { return rgba[0]; }
    float green() const { return rgba[1]; }
    float blue() const { return rgba[2]; }
    float alpha() const { return rgba[3]; }

    const float* data() const { return rgba; }

    float rgba[4];
};

inline Color3::Color3(const Color4& c4) { rgb[0] = c4[0]; rgb[1] = c4[1]; rgb[2] = c4[2]; }

class ByteColor4 : public Color4
{
public:
    ByteColor4() : Color4() {}
};

// --- BSVertexDesc (Skyrim SE / FO4 packed vertex-format descriptor) ---

enum VertexAttribute : u8
{
    VA_POSITION = 0x0,
    VA_TEXCOORD0 = 0x1,
    VA_TEXCOORD1 = 0x2,
    VA_NORMAL = 0x3,
    VA_BINORMAL = 0x4,
    VA_COLOR = 0x5,
    VA_SKINNING = 0x6,
    VA_LANDDATA = 0x7,
    VA_EYEDATA = 0x8,
    VA_COUNT = 9
};

enum VertexFlags : u16
{
    VF_VERTEX = 1 << VA_POSITION,
    VF_UV = 1 << VA_TEXCOORD0,
    VF_UV_2 = 1 << VA_TEXCOORD1,
    VF_NORMAL = 1 << VA_NORMAL,
    VF_TANGENT = 1 << VA_BINORMAL,
    VF_COLORS = 1 << VA_COLOR,
    VF_SKINNED = 1 << VA_SKINNING,
    VF_LANDDATA = 1 << VA_LANDDATA,
    VF_EYEDATA = 1 << VA_EYEDATA,
    VF_FULLPREC = 0x400
};

class BSVertexDesc
{
public:
    BSVertexDesc() = default;

    void SetFlags(VertexFlags flags) { desc = (desc & 0x000000FFFFFFFFFFull) | (static_cast<u64>(flags) << 44); }
    VertexFlags GetFlags() const { return VertexFlags((desc >> 44) & 0xFFFF); }
    bool HasFlag(VertexFlags flag) const { return (GetFlags() & flag) != 0; }
    void SetSize(u32 sizeBytes) { desc = (desc & ~0xFull) | ((static_cast<u64>(sizeBytes) >> 2) & 0xF); }
    u32 GetVertexSize() const { return (desc & 0xF) * 4; }

    bool operator==(const BSVertexDesc& v) const = default;

    u64 desc = 0;
};

} // namespace nsk
