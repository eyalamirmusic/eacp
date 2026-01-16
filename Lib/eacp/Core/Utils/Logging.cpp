#include "Logging.h"

#include <iostream>

namespace eacp
{
void LOG(const std::string& text)
{
    std::cout << text << std::endl;
}
} // namespace eacp
