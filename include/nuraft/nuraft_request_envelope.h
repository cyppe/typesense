#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class NuRaftRequestEnvelope {
public:
    static constexpr uint32_t kMagic = 0x54534e52;  // TSNR
    static constexpr uint16_t kCurrentVersion = 1;
    static constexpr uint16_t kHttpReqJsonEncoding = 1;

    NuRaftRequestEnvelope() = default;
    explicit NuRaftRequestEnvelope(std::string request_json,
                                   uint16_t payload_encoding = kHttpReqJsonEncoding,
                                   uint16_t flags = 0);

    uint16_t version() const;
    uint16_t payload_encoding() const;
    uint16_t flags() const;
    const std::string& request_json() const;

    std::string serialize() const;

    static bool deserialize(std::string_view bytes,
                            NuRaftRequestEnvelope& envelope,
                            std::string& error);

private:
    uint16_t version_ = kCurrentVersion;
    uint16_t payload_encoding_ = kHttpReqJsonEncoding;
    uint16_t flags_ = 0;
    std::string request_json_;
};
