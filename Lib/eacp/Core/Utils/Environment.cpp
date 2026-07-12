#include "Environment.h"

namespace eacp
{

std::string getEnvValue(std::string_view name)
{
    return getEnv(name).value_or(std::string {});
}

} // namespace eacp
