#include <eacp/ObjC/ObjC.h>
#include <eacp/ObjC/AutoReleasePool.h>
#include <iostream>

@interface Logger : NSObject
@end

@implementation Logger

- (instancetype)init
{
    std::cout << "Constructor\n";
    return self;
}

- (void)dealloc
{
    std::cout << "Destructor\n";
    [super dealloc];
}

@end

struct MultiLoggers
{
    using MyLogger = eacp::ObjC::Ptr<Logger>;

    MyLogger a {eacp::ObjC::makePtr<Logger>()};
    MyLogger b {eacp::ObjC::makePtr<Logger>()};
    MyLogger c {eacp::ObjC::makePtr<Logger>()};
};

void runExperiment()
{
    auto s = eacp::ObjC::AutoReleasePool();
    auto rawLogger = MultiLoggers();
    auto other = rawLogger;
}

int main()
{
    runExperiment();
    return 0;
}