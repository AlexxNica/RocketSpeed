// AUTOGENERATED FILE - DO NOT MODIFY!
// This file generated by Djinni from rocketspeed.djinni

#pragma once

#include <cstdint>
#include <vector>

namespace rocketspeed { namespace djinni {

class MessageReceivedCallback {
public:
    virtual ~MessageReceivedCallback() {}

    virtual void Call(int64_t sub_handle, int64_t start_seqno, std::vector<uint8_t> contents) = 0;
};

} }  // namespace rocketspeed::djinni