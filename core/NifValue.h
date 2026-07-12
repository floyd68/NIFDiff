// NifValue.h - Qt-free replacement for src/data/nifvalue.h
//
// Scope note: the original NifValue is a hand-rolled tagged union driven by a
// ~43 entry enum plus a QHash-based type/enum registry populated from
// nif.xml, because it has to represent *any* field that nif.xml can declare
// (needed for the generic editable tree view). The lite viewer does not ship
// a generic nif.xml-driven reflection engine (see NifDocument.h for the
// rationale) - blocks that matter for rendering are parsed straight into
// typed C++ structs. NifValue is kept here as a small, real, Qt-free
// std::variant-based value box for the cases that still want a uniform
// "value of unknown-until-runtime type" container, e.g. NifItem below (used
// for generic browsing/diagnostics of block fields that are not part of the
// curated render path).
#pragma once

#include "NifTypes.h"
#include <string>
#include <variant>
#include <cstdint>

namespace nsk
{

class NifValue
{
public:
    enum class Type
    {
        None, Bool, Int, Float, String, Link, Vector2, Vector3, Vector4,
        Color3, Color4, Matrix33, Matrix44, Quat, Triangle
    };

    using Storage = std::variant<
        std::monostate, bool, std::int64_t, float, std::string,
        Vector2, Vector3, Vector4, Color3, Color4, Matrix, Matrix4, Quat, Triangle>;

    NifValue() = default;
    NifValue(bool v) : m_type(Type::Bool), m_data(v) {}
    NifValue(std::int64_t v) : m_type(Type::Int), m_data(v) {}
    NifValue(float v) : m_type(Type::Float), m_data(v) {}
    NifValue(std::string v) : m_type(Type::String), m_data(std::move(v)) {}
    NifValue(Vector2 v) : m_type(Type::Vector2), m_data(v) {}
    NifValue(Vector3 v) : m_type(Type::Vector3), m_data(v) {}
    NifValue(Vector4 v) : m_type(Type::Vector4), m_data(v) {}
    NifValue(Color3 v) : m_type(Type::Color3), m_data(v) {}
    NifValue(Color4 v) : m_type(Type::Color4), m_data(v) {}
    NifValue(Matrix v) : m_type(Type::Matrix33), m_data(v) {}
    NifValue(Matrix4 v) : m_type(Type::Matrix44), m_data(v) {}
    NifValue(Quat v) : m_type(Type::Quat), m_data(v) {}
    NifValue(Triangle v) : m_type(Type::Triangle), m_data(v) {}

    static NifValue makeLink(std::int32_t idx)
    {
        NifValue v(static_cast<std::int64_t>(idx));
        v.m_type = Type::Link;
        return v;
    }

    Type type() const { return m_type; }
    bool isValid() const { return m_type != Type::None; }

    template <typename T>
    T get() const
    {
        if (auto* p = std::get_if<T>(&m_data))
            return *p;
        return T();
    }

    std::int64_t toInt() const
    {
        if (auto* p = std::get_if<std::int64_t>(&m_data)) return *p;
        if (auto* p = std::get_if<bool>(&m_data)) return *p ? 1 : 0;
        return 0;
    }
    float toFloat() const
    {
        if (auto* p = std::get_if<float>(&m_data)) return *p;
        return 0.0f;
    }
    std::string toString() const
    {
        if (auto* p = std::get_if<std::string>(&m_data)) return *p;
        return {};
    }

    std::string toDisplayString() const;

private:
    Type m_type = Type::None;
    Storage m_data {};
};

inline std::string NifValue::toDisplayString() const
{
    switch (m_type)
    {
    case Type::Bool:   return toInt() ? "true" : "false";
    case Type::Int:
    case Type::Link:   return std::to_string(toInt());
    case Type::Float:  return std::to_string(toFloat());
    case Type::String: return toString();
    case Type::Vector3:
    {
        Vector3 v = get<Vector3>();
        return "(" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", " + std::to_string(v[2]) + ")";
    }
    case Type::Color4:
    {
        Color4 c = get<Color4>();
        return "(" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) + ", " + std::to_string(c[3]) + ")";
    }
    default:
        return {};
    }
}

} // namespace nsk
