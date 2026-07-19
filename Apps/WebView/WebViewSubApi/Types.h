#pragma once

#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

#include <string>

struct Greeting
{
    std::string text;

    MIRO_REFLECT(text)
};

struct GreetRequest
{
    std::string name;

    MIRO_REFLECT(name)
};

struct Tick
{
    int count = 0;

    MIRO_REFLECT(count)
};

namespace Api
{

// The api under test is NESTED, which is the whole point of this fixture.
//
// Miro installs a sub-API under its member identifier, so these reach the wire
// as "nested.greet" / "nested.ticks". A generated TS client, though, is written
// against whatever shape it was generated from — and this app's client calls a
// plain `backend.greet()`, because from its own point of view `greet` is just a
// command at the root.
//
// Closing that gap is exactly what configureBridge({prefix}) in the backend
// template does, and what the tests here exercise end to end over a real
// WebView.
class GreeterApi
{
public:
    Greeting greet(const GreetRequest& req)
    {
        lastGreeted = req.name;
        return Greeting {"hello " + req.name};
    }

    // Lets a test push an event without going through a command, so the
    // subscribe path is covered independently of the invoke path.
    void publishTick(int count) { ticks.publish(Tick {count}); }

    const std::string& greetedName() const { return lastGreeted; }

    Miro::Event<Tick> ticks;

    MIRO_REFLECT_API(greet, ticks)

private:
    std::string lastGreeted;
};

class RootApi
{
public:
    GreeterApi nested;

    MIRO_REFLECT_API(nested)
};

} // namespace Api
