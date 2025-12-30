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

    MyLogger a {[[Logger alloc] init]};
    MyLogger b {[[Logger alloc] init]};
    MyLogger c {[[Logger alloc] init]};
};

void runExperiment()
{
    auto rawLogger = MultiLoggers();
    auto other = rawLogger;
}

int main()
{
    runExperiment();
    return 0;
}