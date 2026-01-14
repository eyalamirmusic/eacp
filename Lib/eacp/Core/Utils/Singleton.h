#pragma once

namespace eacp::Singleton
{
template <typename T>
T& get()
{
    static T object;
    return object;
}
} // namespace eacp::Singleton