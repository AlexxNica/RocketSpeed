// AUTOGENERATED FILE - DO NOT MODIFY!
// This file generated by Djinni from rocketspeed.djinni

#pragma once

#include "HostId.hpp"
#include "djinni_support.hpp"

namespace djinni_generated {

class NativeHostId final {
public:
    using CppType = ::rocketspeed::djinni::HostId;
    using JniType = jobject;

    static jobject toJava(JNIEnv*, ::rocketspeed::djinni::HostId);
    static ::rocketspeed::djinni::HostId fromJava(JNIEnv*, jobject);

    const djinni::GlobalRef<jclass> clazz { djinni::jniFindClass("org/rocketspeed/HostId") };
    const jmethodID jconstructor { djinni::jniGetMethodID(clazz.get(), "<init>", "(Ljava/lang/String;I)V") };
    const jfieldID field_hostname { djinni::jniGetFieldID(clazz.get(), "hostname", "Ljava/lang/String;") };
    const jfieldID field_port { djinni::jniGetFieldID(clazz.get(), "port", "I") };

private:
    NativeHostId() {}
    friend class djinni::JniClass<::djinni_generated::NativeHostId>;
};

}  // namespace djinni_generated