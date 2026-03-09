#include <gtest/gtest.h>

#include <string>

#include "nuraft/nuraft_request_envelope.h"

TEST(NuRaftRequestEnvelopeTest, RoundTripPreservesPayloadAndMetadata) {
    NuRaftRequestEnvelope input("{\"collection\":\"companies\"}", 7, 11);

    const std::string encoded = input.serialize();

    NuRaftRequestEnvelope decoded;
    std::string error;
    ASSERT_TRUE(NuRaftRequestEnvelope::deserialize(encoded, decoded, error)) << error;
    EXPECT_EQ(decoded.version(), NuRaftRequestEnvelope::kCurrentVersion);
    EXPECT_EQ(decoded.payload_encoding(), 7);
    EXPECT_EQ(decoded.flags(), 11);
    EXPECT_EQ(decoded.request_json(), "{\"collection\":\"companies\"}");
}

TEST(NuRaftRequestEnvelopeTest, RejectsInvalidMagic) {
    NuRaftRequestEnvelope input("{}", 1, 0);
    std::string encoded = input.serialize();
    encoded[0] = 0;

    NuRaftRequestEnvelope decoded;
    std::string error;
    ASSERT_FALSE(NuRaftRequestEnvelope::deserialize(encoded, decoded, error));
    EXPECT_EQ(error, "NuRaft request envelope has an invalid magic value");
}

TEST(NuRaftRequestEnvelopeTest, RejectsTruncatedPayload) {
    NuRaftRequestEnvelope input("{\"id\":1}", 1, 0);
    std::string encoded = input.serialize();
    encoded.pop_back();

    NuRaftRequestEnvelope decoded;
    std::string error;
    ASSERT_FALSE(NuRaftRequestEnvelope::deserialize(encoded, decoded, error));
    EXPECT_EQ(error, "NuRaft request envelope payload length does not match the encoded body");
}

TEST(NuRaftRequestEnvelopeTest, RejectsUnsupportedVersion) {
    NuRaftRequestEnvelope input("{}", 1, 0);
    std::string encoded = input.serialize();
    encoded[4] = 2;
    encoded[5] = 0;

    NuRaftRequestEnvelope decoded;
    std::string error;
    ASSERT_FALSE(NuRaftRequestEnvelope::deserialize(encoded, decoded, error));
    EXPECT_EQ(error, "NuRaft request envelope version is unsupported");
}
