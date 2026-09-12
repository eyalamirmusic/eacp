#include <eacp/Core/Core.h>
#include <eacp/Network/Network.h>

using namespace eacp;

// A console app that needs a file from the network asks for it and has it on
// disk when the call returns. The first run downloads it into this app's own
// Application Support folder; later runs find it there and only ask the
// server whether it changed. It throws if the file could not be had.
int main()
{
    auto options = OnlineResource::Options {};
    options.url = "https://media.w3.org/2010/05/sintel/trailer.mp4";

    LOG(OnlineResource::fetch(options).path.str());
}
