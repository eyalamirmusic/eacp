#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace eacp::Crypto
{

class SHA256
{
public:
    SHA256();
    ~SHA256();

    SHA256(const SHA256&) = delete;
    SHA256& operator=(const SHA256&) = delete;
    SHA256(SHA256&&) noexcept;
    SHA256& operator=(SHA256&&) noexcept;

    void update(std::span<const std::uint8_t> data);
    void update(const std::string& text);

    [[nodiscard]] std::string finish();

private:
    struct State;
    std::unique_ptr<State> state;
};

std::string sha256String(const std::string& text);
std::string sha256File(const std::string& path);

} // namespace eacp::Crypto
