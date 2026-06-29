#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc > 1 && std::string(argv[1]) == "--version")
    {
        std::cout << EACP_REAL_UPDATE_DEMO_VERSION << "\n";
        return 0;
    }

    std::cout << "Tamber Local Update Demo "
              << EACP_REAL_UPDATE_DEMO_VERSION << "\n";
    return 0;
}
