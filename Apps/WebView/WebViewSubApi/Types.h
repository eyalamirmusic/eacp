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

// Sub-APIs reach the wire under their member name ("nested.greet"), while the
// generated client calls a root-level greet() — configureBridge({prefix}) in
// the backend template closes that gap.
class GreeterApi
{
public:
    Greeting greet(const GreetRequest& req)
    {
        lastGreeted = req.name;
        return Greeting {"hello " + req.name};
    }

    // Lets a test drive the subscribe path independently of the invoke path.
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
