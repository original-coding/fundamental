#pragma once
#include "fundamental/basic/utils.hpp"
#include "endian_utils.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>


#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
namespace Fundamental
{
// NOTE: This buffer owns the life time of a block of raw memory.
template <typename _SizeType = std::size_t>
class Buffer {
public:
    using SizeType = _SizeType;

public:
    Buffer() = default;

    explicit Buffer(_SizeType sizeInBytes);

    // NOTE: this method is same as std::vector::assign,
    // so data will be copied.
    explicit Buffer(const void* pData, _SizeType sizeInBytes);
    explicit Buffer(const char* cStr);

    ~Buffer();

    // overwrite copy
    Buffer(const Buffer&);
    Buffer& operator=(const Buffer&);

    // Enable move
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;

    // when it allocated failed,an runtime error exception will be thrown
    // when you AttachRawMemory into buffer and you want to Reallocate for new size
    // you need to decide freeOriginalData to be true all false
    template <size_t AlignSize_ = 4, typename = std::enable_if_t<(AlignSize_ % 4 == 0 || AlignSize_ % 8 == 0), int>>
    void Reallocate(_SizeType sizeInBytes, bool freeOriginalData = true) {
        if (m_byteSize == sizeInBytes) return;
        if (sizeInBytes == 0) {
            FreeBuffer();
            return;
        }
        if (freeOriginalData) FreeBuffer();
        auto fixedSize       = AlignSize_ - (sizeInBytes % AlignSize_);
        std::uint8_t* newPtr = reinterpret_cast<std::uint8_t*>(std::realloc(m_pRaw, sizeInBytes + fixedSize));
        if (!newPtr) throw std::runtime_error(std::string("bad alloc for ") + std::to_string(sizeInBytes) + " bytes");
        if (fixedSize > 0) {
            std::memset(newPtr + sizeInBytes, 0, fixedSize);
        }

        m_pRaw     = newPtr;
        m_byteSize = sizeInBytes;
    }

    // NOTE: this method is same as std::vector::assign,
    // so data will be copied.
    // if you do not wish a copy, consider to use MemoryOwner instead
    void AssignBuffer(const void* pData, _SizeType sizeInBytes);

    // This method will free current buffer
    // and assign the raw pointer and size directly without copy
    // NOTE: Assume pData is capable to be released by std::free
    void AttachRawMemory(std::uint8_t* pData, _SizeType sizeInBytes);

    // You detach the raw memory's ownership by this method
    // raw memory is allocated by std::malloc
    // so you need to free the memory by std::free
    void DetachRawMemory(std::uint8_t** ppOutRawData, _SizeType& outSizeInBytes);

    SizeType GetSize() const {
        return m_byteSize;
    }

    std::uintptr_t GetAddress() const;
    std::uint8_t* GetData() const;

    void FreeBuffer();

    operator bool() const;

    bool operator==(const std::string& str) {
        if (m_byteSize != str.size()) return false;
        return 0 == std::memcmp(m_pRaw, str.data(), str.size());
    }

    bool operator!=(const std::string& str) {
        return !(operator==(str));
    }

    bool operator==(const Buffer& buffer) {
        if (m_byteSize != buffer.GetSize()) return false;
        return 0 == std::memcmp(m_pRaw, buffer.GetData(), buffer.GetSize());
    }

    bool operator!=(const Buffer& buffer) {
        return !(operator==(buffer));
    }

    Buffer(const std::string& str) {
        AssignBuffer(reinterpret_cast<const std::uint8_t*>(str.data()), str.size());
    }

    Buffer& operator=(const std::string& str) {
        AssignBuffer(reinterpret_cast<const std::uint8_t*>(str.data()), str.size());
        return *this;
    }

    Buffer& operator=(const char* cStr) {
        AssignBuffer(cStr, strlen(cStr));
        return *this;
    }

    std::string ToString() const {
        const char* ptr = reinterpret_cast<char*>(m_pRaw);
        return ptr ? std::string(ptr, ptr + m_byteSize) : std::string();
    }

    std::vector<std::uint8_t> ToVec() const {
        return m_pRaw ? std::vector<std::uint8_t>(m_pRaw, m_pRaw + m_byteSize) : std::vector<std::uint8_t>();
    }

    std::string ToHexString() const;

    std::string Dump() const {
        std::stringstream ss;
        ss << "Buffer(" << m_byteSize << "):" << ToHexString();
        return ss.str();
    }

    std::string DumpAscii() const {
        return Utils::BufferDumpAscii(m_pRaw, m_byteSize);
    }

private:
    void Reset();

private:
    _SizeType m_byteSize = 0;
    std::uint8_t* m_pRaw = nullptr;
};

template <typename _SizeType>
Buffer<_SizeType>::Buffer(Buffer&& other) noexcept : m_byteSize(other.m_byteSize), m_pRaw(other.m_pRaw) {
    if (&other == this) return;
    other.Reset();
}

template <typename _SizeType>
inline Buffer<_SizeType>& Buffer<_SizeType>::operator=(Buffer<_SizeType>&& other) noexcept {
    if (&other == this) return *this;
    FreeBuffer();

    m_pRaw     = other.m_pRaw;
    m_byteSize = other.m_byteSize;

    other.Reset();
    return *this;
}

template <typename _SizeType>
Buffer<_SizeType>::Buffer(_SizeType sizeInBytes) {
    Reallocate(sizeInBytes);
}

template <typename _SizeType>
Buffer<_SizeType>::Buffer(const void* pData, _SizeType sizeInBytes) {
    AssignBuffer(pData, sizeInBytes);
}

template <typename _SizeType>
inline Buffer<_SizeType>::Buffer(const char* cStr) {
    AssignBuffer(cStr, strlen(cStr));
}

template <typename _SizeType>
Buffer<_SizeType>::~Buffer() {
    FreeBuffer();
}

template <typename _SizeType>
inline Buffer<_SizeType>::Buffer(const Buffer& other) {
    if (&other == this) return;
    AssignBuffer(other.m_pRaw, other.m_byteSize);
}

template <typename _SizeType>
inline Buffer<_SizeType>& Buffer<_SizeType>::operator=(const Buffer& other) {
    if (&other == this) return *this;
    AssignBuffer(other.m_pRaw, other.m_byteSize);
    return *this;
}

template <typename _SizeType>
void Buffer<_SizeType>::AssignBuffer(const void* pData, _SizeType sizeInBytes) {
    Reallocate(sizeInBytes);
    std::memcpy(m_pRaw, pData, m_byteSize);
}

template <typename _SizeType>
void Buffer<_SizeType>::AttachRawMemory(std::uint8_t* pData, _SizeType sizeInBytes) {
    FreeBuffer();

    m_pRaw     = pData;
    m_byteSize = sizeInBytes;
}

template <typename _SizeType>
void Buffer<_SizeType>::DetachRawMemory(std::uint8_t** ppOutRawData, _SizeType& outSizeInBytes) {
    if (ppOutRawData == nullptr) return;

    *ppOutRawData  = m_pRaw;
    outSizeInBytes = m_byteSize;
    Reset();
}

template <typename _SizeType>
std::uintptr_t Buffer<_SizeType>::GetAddress() const {
    return reinterpret_cast<std::uintptr_t>(m_pRaw);
}

template <typename _SizeType>
std::uint8_t* Buffer<_SizeType>::GetData() const {
    return m_pRaw;
}

template <typename _SizeType>
void Buffer<_SizeType>::FreeBuffer() {
    if (operator bool()) {
        std::free(m_pRaw);
    }
    Reset();
}

template <typename _SizeType>
Fundamental::Buffer<_SizeType>::operator bool() const {
    return m_pRaw && m_byteSize > 0;
}

template <typename _SizeType>
inline std::string Buffer<_SizeType>::ToHexString() const {
    return Utils::BufferToHex(m_pRaw, m_byteSize);
}

template <typename _SizeType>
void Buffer<_SizeType>::Reset() {
    m_pRaw     = nullptr;
    m_byteSize = 0;
}

template <typename T>
struct BufferHash {
    std::size_t operator()(const Buffer<T>& buf) const noexcept {
        std::size_t seed = 0;
        auto size        = buf.GetSize();
        auto ptr         = buf.GetData();
        for (typename Buffer<T>::SizeType i = 0; i < size; ++i) {
            seed ^= static_cast<std::size_t>(ptr[i]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

template <typename SizeType   = std::size_t,
          Endian targetEndian = Endian::LittleEndian,
          typename            = std::enable_if_t<std::is_unsigned_v<SizeType>>>
struct BufferReader {
    using ConvertFlag = std::bool_constant<NeedConvertEndian<targetEndian>()>;

public:
    using SizeValueType = SizeType;
    BufferReader() {
    }

    // Disable copy
    BufferReader(const BufferReader&)            = delete;
    BufferReader& operator=(const BufferReader&) = delete;

    // Enable move
    BufferReader(BufferReader&& other) noexcept {
        operator=(std::move(other));
    }
    BufferReader& operator=(BufferReader&& other) {
        m_pRawBuffer       = other.m_pRawBuffer;
        other.m_pRawBuffer = nullptr;

        m_bufferSize      = other.m_bufferSize;
        m_currentPosition = other.m_currentPosition;
        return *this;
    }

    void Reset() {
        m_pRawBuffer      = nullptr;
        m_bufferSize      = static_cast<SizeType>(-1);
        m_currentPosition = 0;
    }

    void SetBuffer(const std::uint8_t* buf, SizeType len) {
        m_pRawBuffer      = buf;
        m_bufferSize      = len;
        m_currentPosition = 0;
    }

    SizeType GetCurrentPosition() const {
        return m_currentPosition;
    }
    SizeType GetBufferSize() const {
        return m_bufferSize;
    }
    const std::uint8_t* GetBuffer() const {
        return m_pRawBuffer;
    }

    // Read buffer to pDest and offset the current position.
    template <typename ValueType>
    void ReadValue(ValueType* pDest, SizeType valueSize = sizeof(ValueType)) {
        PeekValue(pDest, m_currentPosition, valueSize);
        m_currentPosition += valueSize;
    }

    void PeekOffset(SizeType offset) {
        m_currentPosition += offset;
    }

    // Read buffer to pDest and offset the current position.
    template <typename ValueType>
    void PeekValue(ValueType* pDest, SizeType srcBufferOffset = 0, SizeType valueSize = sizeof(ValueType)) {
        if (srcBufferOffset + valueSize > m_bufferSize) {
            std::string ex = std::string("buffer overflow");
            ex += " pos:" + std::to_string(srcBufferOffset) + " need:" + std::to_string(valueSize) +
                  " max:" + std::to_string(m_bufferSize);
            throw std::invalid_argument(ex);
        }

        constexpr static std::size_t kValueSize = sizeof(ValueType);
        if constexpr ((kValueSize > 1) && std::conjunction_v<std::is_integral<ValueType>, ConvertFlag>) {
            auto pData                = m_pRawBuffer + srcBufferOffset;
            std::uint8_t* pDestBuffer = reinterpret_cast<std::uint8_t*>(pDest);
            for (SizeType i = 0; i < valueSize; ++i) {
                pDestBuffer[i] = pData[valueSize - 1 - i];
            }
        } else {
            std::memcpy(pDest, m_pRawBuffer + srcBufferOffset, valueSize);
        }
    }

    template <typename VectorLikeType>
    void ReadVectorLike(VectorLikeType& vectorLike) {
        SizeType size = 0;
        ReadValue(&size);
        if (size + m_currentPosition > m_bufferSize) {
            std::string ex = std::string("vec buffer overflow");
            ex += " pos:" + std::to_string(m_currentPosition) + " need:" + std::to_string(size) +
                  " max:" + std::to_string(m_bufferSize);
            throw std::invalid_argument(ex);
        }
        vectorLike.resize(size / sizeof(typename VectorLikeType::value_type));

        if (size > 0) {
            ReadValue(reinterpret_cast<std::uint8_t*>(vectorLike.data()), size);
        }
    }

    template <typename EnumType, typename = std::enable_if_t<std::is_enum_v<EnumType>>>
    void ReadEnum(EnumType& destEnum) {
        std::underlying_type_t<EnumType> intValue;
        ReadValue(&intValue);
        destEnum = static_cast<EnumType>(intValue);
    }

    void ReadRawMemory(Buffer<SizeType>& destRawBuffer) {
        SizeType size = 0;
        ReadValue(&size);
        if (size + m_currentPosition > m_bufferSize) {
            std::string ex = std::string("raw buffer overflow");
            ex += " pos:" + std::to_string(m_currentPosition) + " need:" + std::to_string(size) +
                  " max:" + std::to_string(m_bufferSize);
            throw std::invalid_argument(ex);
        }
        destRawBuffer.Reallocate(size);

        if (size > 0) {
            ReadValue(destRawBuffer.GetData(), size);
        }
    }

    template <typename VectorLikeType>
    constexpr static SizeType GetVectorLikeSize(const VectorLikeType& vectorLike) {
        return sizeof(SizeType) +
               static_cast<SizeType>(vectorLike.size() * sizeof(typename VectorLikeType::value_type));
    }

private:
    const std::uint8_t* m_pRawBuffer = nullptr;
    SizeType m_bufferSize            = static_cast<SizeType>(-1);
    SizeType m_currentPosition       = 0;
};

// Fixed buffer writer
template <typename SizeType   = std::size_t,
          Endian targetEndian = Endian::LittleEndian,
          typename            = std::enable_if_t<std::is_unsigned_v<SizeType>>>
class BufferWriter {
    using ConvertFlag = std::bool_constant<NeedConvertEndian<targetEndian>()>;

public:
    BufferWriter() {
    }

    // Disable copy
    BufferWriter(const BufferWriter&)            = delete;
    BufferWriter& operator=(const BufferWriter&) = delete;

    // Enable move
    BufferWriter(BufferWriter&& other) noexcept {
        operator=(std::move(other));
    }
    BufferWriter& operator=(BufferWriter&& other) {
        m_pRawBuffer       = other.m_pRawBuffer;
        other.m_pRawBuffer = nullptr;

        m_bufferSize      = other.m_bufferSize;
        m_currentPosition = other.m_currentPosition;
        return *this;
    }

    void Reset() {
        m_pRawBuffer      = nullptr;
        m_bufferSize      = static_cast<SizeType>(-1);
        m_currentPosition = 0;
    }

    void SetBuffer(std::uint8_t* buf, SizeType len) {
        m_pRawBuffer      = buf;
        m_bufferSize      = len;
        m_currentPosition = 0;
    }

    SizeType GetCurrentPosition() const {
        return m_currentPosition;
    }
    SizeType GetBufferSize() const {
        return m_bufferSize;
    }
    const std::uint8_t* GetBuffer() const {
        return m_pRawBuffer;
    }

    template <typename ValueType>
    void WriteValue(ValueType* pValue, SizeType valueSize = sizeof(ValueType)) {
        constexpr static std::size_t kValueSize = sizeof(ValueType);
        if constexpr ((kValueSize > 1) && std::conjunction_v<std::is_integral<ValueType>, ConvertFlag>) {
            auto pDest               = m_pRawBuffer + m_currentPosition;
            const std::uint8_t* pSrc = reinterpret_cast<const std::uint8_t*>(pValue);
            for (SizeType i = 0; i < valueSize; ++i) {
                pDest[i] = pSrc[valueSize - 1 - i];
            }
        } else {
            std::memcpy(m_pRawBuffer + m_currentPosition, pValue, valueSize);
        }
        m_currentPosition += valueSize;
    }

    template <typename VectorLikeType>
    void WriteVectorLike(const VectorLikeType& vectorLike) {
        auto size = static_cast<SizeType>(vectorLike.size() * sizeof(typename VectorLikeType::value_type));
        WriteValue(&size);
        if (size > 0) {
            WriteValue((std::uint8_t*)vectorLike.data(), size);
        }
    }

    template <typename EnumType, typename = std::enable_if_t<std::is_enum_v<EnumType>>>
    void WriteEnum(EnumType eValue) {
        auto intValue = static_cast<std::underlying_type_t<EnumType>>(eValue);
        WriteValue(&intValue);
    }

    void WriteRawMemory(const Buffer<SizeType>& srcRawBuffer) {
        auto size = srcRawBuffer.GetSize();
        WriteValue(&size);
        if (size > 0) {
            WriteValue(srcRawBuffer.GetData(), size);
        }
    }

private:
    std::uint8_t* m_pRawBuffer = nullptr;
    SizeType m_bufferSize      = static_cast<SizeType>(-1);
    SizeType m_currentPosition = 0;
};
} // namespace Fundamental
