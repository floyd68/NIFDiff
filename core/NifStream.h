// NifStream.h - Qt-free replacement for src/io/nifstream.h
//
// The original NifIStream/NifOStream pair wraps a QIODevice + QDataStream and
// is driven generically by NifValue::Type through the (Qt) model. Since the
// lite viewer's data core (Phase 2) parses a curated set of block types
// directly into C++ structs (see NifBlocks.h) rather than through a generic
// nif.xml-interpreted tree, NifStream here is simplified to a flat, ordinary
// little-endian binary reader over an in-memory byte buffer using only the
// standard library. All NIF versions the lite viewer targets (Skyrim/SE/FO4)
// are little-endian, so no endian-swap path is implemented.
#pragma once

#include "NifTypes.h"
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>
#include <stdexcept>

namespace nsk
{

// The reader below memcpy's little-endian file bytes straight into host
// integers/floats with no swap path (see the file comment). Turn that
// assumption into a compile error on a big-endian target instead of
// silently mis-reading every field.
static_assert(std::endian::native == std::endian::little,
    "NifIStream assumes a little-endian host; add byte swapping before porting");

class NifIStream
{
public:
    // Accepts anything contiguous over bytes (std::vector<std::uint8_t>
    // converts implicitly). The stream is a non-owning view; the buffer must
    // outlive it.
    NifIStream(std::span<const std::uint8_t> data)
        : m_data(data.data()), m_size(data.size()), m_pos(0)
    {
    }

    std::size_t pos() const { return m_pos; }
    std::size_t size() const { return m_size; }
    bool atEnd() const { return m_pos >= m_size; }
    bool canRead(std::size_t n) const { return m_pos + n <= m_size; }

    void seek(std::size_t pos) { m_pos = pos; }
    void skip(std::size_t n) { m_pos += n; if (m_pos > m_size) m_pos = m_size; }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    T read()
    {
        T v{};
        readRaw(&v, sizeof(T));
        return v;
    }

    void readRaw(void* out, std::size_t n)
    {
        if (!canRead(n))
        {
            std::memset(out, 0, n);
            m_pos = m_size;
            m_eof = true;
            return;
        }
        std::memcpy(out, m_data + m_pos, n);
        m_pos += n;
    }

    std::uint8_t  u8()  { return read<std::uint8_t>(); }
    std::uint16_t u16() { return read<std::uint16_t>(); }
    std::uint32_t u32() { return read<std::uint32_t>(); }
    std::uint64_t u64() { return read<std::uint64_t>(); }
    std::int32_t  i32() { return read<std::int32_t>(); }
    float         f32() { return read<float>(); }
    bool          boolean() { return u8() != 0; }

    // NIF "short string": one length byte followed by that many raw chars.
    std::string shortString()
    {
        std::uint8_t len = u8();
        std::string s;
        s.resize(len);
        if (len)
            readRaw(s.data(), len);
        return s;
    }

    // Header-style line string terminated by '\n' (used by the NIF magic/version line).
    std::string lineString()
    {
        std::string s;
        while (!atEnd())
        {
            char c = static_cast<char>(u8());
            if (c == '\n')
                break;
            s.push_back(c);
        }
        return s;
    }

    // Sized string: 4-byte length prefix followed by raw (non null-terminated) chars.
    std::string sizedString()
    {
        std::uint32_t len = u32();
        std::string s;
        if (len && len < (16u * 1024u * 1024u))
        {
            s.resize(len);
            readRaw(s.data(), len);
        }
        return s;
    }

    Vector2 vector2() { return Vector2(f32(), f32()); }
    Vector3 vector3() { return Vector3(f32(), f32(), f32()); }
    Vector4 vector4() { return Vector4(f32(), f32(), f32(), f32()); }
    Color3  color3()  { return Color3(f32(), f32(), f32()); }
    Color4  color4()  { return Color4(f32(), f32(), f32(), f32()); }
    Triangle triangle() { return Triangle(u16(), u16(), u16()); }

    Matrix matrix33()
    {
        Matrix m;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                m(r, c) = f32();
        return m;
    }

    Quat quatWXYZ() { return Quat(f32(), f32(), f32(), f32()); }

    bool eof() const { return m_eof; }

private:
    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos;
    bool m_eof = false;
};

} // namespace nsk
