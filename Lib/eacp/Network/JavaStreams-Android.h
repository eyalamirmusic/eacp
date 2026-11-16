#pragma once

#include <eacp/Core/Android/Jni.h>

namespace eacp::Network
{
struct JavaStreams
{
    void resolve(Jni::Lookup& java)
    {
        inputStream = java.findClass("java/io/InputStream");
        outputStream = java.findClass("java/io/OutputStream");

        read = java.method(inputStream, "read", "([B)I");
        closeInput = java.method(inputStream, "close", "()V");
        writeWholeArray = java.method(outputStream, "write", "([B)V");
        writeArrayRange = java.method(outputStream, "write", "([BII)V");
        closeOutput = java.method(outputStream, "close", "()V");
    }

    jclass inputStream = nullptr;
    jclass outputStream = nullptr;

    jmethodID read = nullptr;
    jmethodID closeInput = nullptr;
    jmethodID writeWholeArray = nullptr;
    jmethodID writeArrayRange = nullptr;
    jmethodID closeOutput = nullptr;
};
} // namespace eacp::Network
