#include <eacp/Network/Http.h>
#include <iostream>

int main()
{
    auto req = eacp::HTTP::Request();
    req.url = "https://www.google.com";
    auto res = eacp::HTTP::httpRequest(req);
    std::cout << res.content << std::endl;

    return 0;
}