#include "Options.h"

namespace eacp::ML
{
Result Result::success()
{
    return {true, {}};
}

Result Result::failure(const std::string& message)
{
    return {false, message};
}
} // namespace eacp::ML
