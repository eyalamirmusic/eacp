#include <eacp/ResourceEmbed/ResourceEmbed.h>

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: ResourcesEmbed <file_path> "
                     "<resource_name>\n";
        return 1;
    }

    auto code = eacp::ResourceEmbed::generateEmbedCodeFromFile(argv[1], argv[2]);
    std::cout << code;
    return 0;
}
