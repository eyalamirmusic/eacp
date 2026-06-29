#include <eacp/Updater/Updater.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc > 1 && std::string(argv[1]) == "--version")
    {
        std::cout << EACP_APPHUB_HELPER_LABEL << " 1.0.0\n";
        return 0;
    }

    std::cout << EACP_APPHUB_HELPER_LABEL
              << " installed. IPC command handling is not implemented yet.\n";
    return 0;
}
