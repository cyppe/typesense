#include "nuraft/nuraft_request_envelope.h"

#include <cstring>
#include <type_traits>

namespace {

constexpr size_t kHeaderSize = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                               sizeof(uint16_t) + sizeof(uint64_t);

template <typename T>
void append_le(std::string& out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

template <typename T>
bool read_le(std::string_view bytes, size_t offset, T& value) {
    if (offset + sizeof(T) > bytes.size()) {
        return false;
    }

    using UnsignedT = typename std::make_unsigned<T>::type;
    UnsignedT result = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        result |= static_cast<UnsignedT>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
    }
    value = static_cast<T>(result);
    return true;
}

}  // namespace

NuRaftRequestEnvelope::NuRaftRequestEnvelope(std::string request_json,
                                             uint16_t payload_encoding,
                                             uint16_t flags)
    : payload_encoding_(payload_encoding), flags_(flags), request_json_(std::move(request_json)) {}

uint16_t NuRaftRequestEnvelope::version() const {
    return version_;
}

uint16_t NuRaftRequestEnvelope::payload_encoding() const {
    return payload_encoding_;
}

uint16_t NuRaftRequestEnvelope::flags() const {
    return flags_;
}

const std::string& NuRaftRequestEnvelope::request_json() const {
    return request_json_;
}

std::string NuRaftRequestEnvelope::serialize() const {
    std::string out;
    out.reserve(kHeaderSize + request_json_.size());

    append_le<uint32_t>(out, kMagic);
    append_le<uint16_t>(out, version_);
    append_le<uint16_t>(out, payload_encoding_);
    append_le<uint16_t>(out, flags_);
    append_le<uint64_t>(out, request_json_.size());
    out.append(request_json_);

    return out;
}

bool NuRaftRequestEnvelope::deserialize(std::string_view bytes,
                                        NuRaftRequestEnvelope& envelope,
                                        std::string& error) {
    if (bytes.size() < kHeaderSize) {
        error = "NuRaft request envelope is truncated before the header is complete";
        return false;
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t payload_encoding = 0;
    uint16_t flags = 0;
    uint64_t payload_size = 0;

    if (!read_le<uint32_t>(bytes, 0, magic) ||
        !read_le<uint16_t>(bytes, sizeof(uint32_t), version) ||
        !read_le<uint16_t>(bytes, sizeof(uint32_t) + sizeof(uint16_t), payload_encoding) ||
        !read_le<uint16_t>(bytes, sizeof(uint32_t) + (2 * sizeof(uint16_t)), flags) ||
        !read_le<uint64_t>(bytes, sizeof(uint32_t) + (3 * sizeof(uint16_t)), payload_size)) {
        error = "NuRaft request envelope header could not be decoded";
        return false;
    }

    if (magic != kMagic) {
        error = "NuRaft request envelope has an invalid magic value";
        return false;
    }

    if (version != kCurrentVersion) {
        error = "NuRaft request envelope version is unsupported";
        return false;
    }

    if (payload_size != bytes.size() - kHeaderSize) {
        error = "NuRaft request envelope payload length does not match the encoded body";
        return false;
    }

    envelope.version_ = version;
    envelope.payload_encoding_ = payload_encoding;
    envelope.flags_ = flags;
    envelope.request_json_ = std::string(bytes.substr(kHeaderSize));
    error.clear();
    return true;
}
