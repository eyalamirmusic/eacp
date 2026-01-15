#include "Strings.h"

namespace eacp::Strings
{

NSData* toNSData(const std::string& input)
{
    return [NSData dataWithBytes:input.data() length:input.length()];
}

NSString* toNSString(const std::string& input)
{
    return [NSString stringWithUTF8String:input.c_str()];
}

std::string toStdString(NSError* error)
{
    if (!error)
        return {};

    return [error.localizedDescription UTF8String];
}

std::string toStdString(NSData* data)
{
    if (data == nullptr || data.length == 0)
        return {};

    return {(const char*) data.bytes, data.length};
}
}