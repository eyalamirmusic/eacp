#include <eacp/Core/ObjC/ObjC.h>
#include <NanoTest/NanoTest.h>

#import <Foundation/Foundation.h>
#include <atomic>

using namespace nano;

template <typename T>
using OPtr = eacp::ObjC::Ptr<T>;

using eacp::ObjC::RetainMode;
using eacp::ObjC::alloc;
using eacp::ObjC::createNew;
using eacp::ObjC::attachPtr;
using eacp::ObjC::makePtr;

namespace counted
{
std::atomic<int> alive {0};
std::atomic<int> retains {0};
std::atomic<int> releases {0};

void reset()
{
    alive = 0;
    retains = 0;
    releases = 0;
}
} // namespace counted

@interface Counted : NSObject
{
@public
    NSUInteger trackedRetainCount;
}
@end

@implementation Counted
- (instancetype)init
{
    if ((self = [super init]))
    {
        trackedRetainCount = 1;
        counted::alive.fetch_add(1);
    }
    return self;
}

- (instancetype)retain
{
    counted::retains.fetch_add(1);
    trackedRetainCount++;
    return self;
}

- (oneway void)release
{
    counted::releases.fetch_add(1);
    if (trackedRetainCount <= 1)
    {
        [self dealloc];
        return;
    }
    trackedRetainCount--;
}

- (NSUInteger)retainCount
{
    return trackedRetainCount;
}

- (void)dealloc
{
    counted::alive.fetch_sub(1);
    [super dealloc];
}
@end

auto tDefaultCtor = test("ObjCPtr/defaultCtorIsNullAndFalsy") = []
{
    OPtr<Counted> p;
    check(p.get() == nullptr);
    check(!static_cast<bool>(p));
    check(p == (Counted*) nullptr);
};

auto tOwnTransfer = test("ObjCPtr/rawCtorAdoptsWithoutRetain") = []
{
    counted::reset();
    {
        auto* obj = [[Counted alloc] init];
        check(obj.retainCount == 1);
        OPtr<Counted> p(obj);
        check(p.get() == obj);
        check(obj.retainCount == 1);
        check(counted::retains.load() == 0);
    }
    check(counted::releases.load() == 1);
    check(counted::alive.load() == 0);
};

auto tRetainCtor = test("ObjCPtr/retainModeCtorRetains") = []
{
    counted::reset();
    auto* obj = [[Counted alloc] init];
    {
        OPtr<Counted> p(obj, RetainMode {});
        check(p.get() == obj);
        check(obj.retainCount == 2);
        check(counted::retains.load() == 1);
    }
    check(obj.retainCount == 1);
    [obj release];
    check(counted::alive.load() == 0);
};

auto tCopyCtor = test("ObjCPtr/copyCtorRetains") = []
{
    counted::reset();
    {
        auto* obj = [[Counted alloc] init];
        OPtr<Counted> p1(obj);
        OPtr<Counted> p2(p1);
        check(p1.get() == p2.get());
        check(obj.retainCount == 2);
    }
    check(counted::alive.load() == 0);
};

auto tDestructorReleases = test("ObjCPtr/destructorReleases") = []
{
    counted::reset();
    {
        OPtr<Counted> p([[Counted alloc] init]);
        check(counted::alive.load() == 1);
    }
    check(counted::alive.load() == 0);
};

auto tReleaseMethod = test("ObjCPtr/releaseMethodNullsAndFrees") = []
{
    counted::reset();
    OPtr<Counted> p([[Counted alloc] init]);
    check(counted::alive.load() == 1);
    p.release();
    check(p.get() == nullptr);
    check(counted::alive.load() == 0);
};

auto tReleaseOnNullSafe = test("ObjCPtr/releaseOnNullIsNoop") = []
{
    counted::reset();
    OPtr<Counted> p;
    p.release();
    check(p.get() == nullptr);
    check(counted::releases.load() == 0);
};

auto tSetReplaces = test("ObjCPtr/setReleasesOldAdoptsNewWithoutRetain") = []
{
    counted::reset();
    {
        auto* a = [[Counted alloc] init];
        auto* b = [[Counted alloc] init];
        OPtr<Counted> p(a);
        check(counted::alive.load() == 2);
        p.set(b);
        check(p.get() == b);
        check(counted::alive.load() == 1);
        check(b.retainCount == 1);
        check(counted::retains.load() == 0);
    }
    check(counted::alive.load() == 0);
};

auto tSetSamePtrIsNoop = test("ObjCPtr/setOnSamePointerDoesNotDoubleRelease") = []
{
    counted::reset();
    {
        auto* obj = [[Counted alloc] init];
        OPtr<Counted> p(obj);
        p.set(obj);
        check(p.get() == obj);
        check(obj.retainCount == 1);
        check(counted::releases.load() == 0);
    }
    check(counted::alive.load() == 0);
};

auto tRetainMethod = test("ObjCPtr/retainIncrementsCount") = []
{
    counted::reset();
    auto* obj = [[Counted alloc] init];
    OPtr<Counted> p(obj);
    p.retain();
    check(obj.retainCount == 2);
    [obj release];
    check(counted::alive.load() == 1);
    p.release();
    check(counted::alive.load() == 0);
};

auto tRetainOnNullSafe = test("ObjCPtr/retainOnNullIsNoop") = []
{
    counted::reset();
    OPtr<Counted> p;
    p.retain();
    check(counted::retains.load() == 0);
};

auto tResetReplaces = test("ObjCPtr/resetReleasesOldRetainsNew") = []
{
    counted::reset();
    auto* a = [[Counted alloc] init];
    auto* b = [[Counted alloc] init];
    {
        OPtr<Counted> p(a);
        p.reset(b);
        check(p.get() == b);
        check(b.retainCount == 2);
        check(counted::alive.load() == 1);
    }
    check(b.retainCount == 1);
    [b release];
    check(counted::alive.load() == 0);
};

auto tResetSameIsNoop = test("ObjCPtr/resetOnSamePointerIsNoop") = []
{
    counted::reset();
    {
        auto* obj = [[Counted alloc] init];
        OPtr<Counted> p(obj);
        p.reset(obj);
        check(obj.retainCount == 1);
        check(counted::retains.load() == 0);
        check(counted::releases.load() == 0);
    }
    check(counted::alive.load() == 0);
};

auto tAssignFromRaw = test("ObjCPtr/assignFromRawAdoptsWithoutRetain") = []
{
    counted::reset();
    {
        auto* a = [[Counted alloc] init];
        auto* b = [[Counted alloc] init];
        OPtr<Counted> p(a);
        check(counted::alive.load() == 2);
        p = b;
        check(p.get() == b);
        check(counted::alive.load() == 1);
        check(b.retainCount == 1);
    }
    check(counted::alive.load() == 0);
};

auto tAssignFromPtr = test("ObjCPtr/assignFromPtrRetains") = []
{
    counted::reset();
    {
        auto* obj = [[Counted alloc] init];
        OPtr<Counted> p1(obj);
        OPtr<Counted> p2;
        p2 = p1;
        check(p1.get() == p2.get());
        check(obj.retainCount == 2);
    }
    check(counted::alive.load() == 0);
};

auto tAssignFromPtrOverwrites = test("ObjCPtr/assignFromPtrReleasesOld") = []
{
    counted::reset();
    {
        auto* a = [[Counted alloc] init];
        auto* b = [[Counted alloc] init];
        OPtr<Counted> p1(a);
        OPtr<Counted> p2(b);
        p1 = p2;
        check(p1.get() == b);
        check(p2.get() == b);
        check(b.retainCount == 2);
        check(counted::alive.load() == 1);
    }
    check(counted::alive.load() == 0);
};

auto tBoolConversion = test("ObjCPtr/boolConversion") = []
{
    counted::reset();
    {
        OPtr<Counted> empty;
        OPtr<Counted> nonEmpty([[Counted alloc] init]);
        check(!empty);
        check(static_cast<bool>(nonEmpty));
    }
    check(counted::alive.load() == 0);
};

auto tRawEquality = test("ObjCPtr/rawPointerEquality") = []
{
    counted::reset();
    {
        auto* a = [[Counted alloc] init];
        auto* b = [[Counted alloc] init];
        OPtr<Counted> pA(a);
        OPtr<Counted> pB(b);
        check(pA == a);
        check(!(pA == b));
        check(pA != b);
        check(!(pA != a));
    }
    check(counted::alive.load() == 0);
};

auto tPtrEqualitySamePointer = test("ObjCPtr/ptrEqualitySamePointerIsEqual") = []
{
    counted::reset();
    auto* obj = [[Counted alloc] init];
    {
        OPtr<Counted> p1(obj, RetainMode {});
        OPtr<Counted> p2(obj, RetainMode {});
        check(p1 == p2);
        check(!(p1 != p2));
    }
    [obj release];
    check(counted::alive.load() == 0);
};

auto tPtrEqualityDifferent = test("ObjCPtr/ptrEqualityDifferentPointersAreNotEqual") = []
{
    counted::reset();
    {
        OPtr<Counted> p1([[Counted alloc] init]);
        OPtr<Counted> p2([[Counted alloc] init]);
        check(!(p1 == p2));
        check(p1 != p2);
    }
    check(counted::alive.load() == 0);
};

auto tArrowReachesObject = test("ObjCPtr/arrowOperatorReturnsObjectPointer") = []
{
    @autoreleasepool
    {
        auto* hello = [[NSString alloc] initWithUTF8String:"hello"];
        OPtr<NSString> p(hello);
        check(p.operator->() == hello);
        check([p.operator->() isEqualToString:@"hello"]);
    }
};

auto tConstGetAccess = test("ObjCPtr/constGetReturnsSamePointer") = []
{
    @autoreleasepool
    {
        auto* hello = [[NSString alloc] initWithUTF8String:"hello"];
        const OPtr<NSString> p(hello);
        check(p.get() == hello);
    }
};

auto tIsKindOfClassMatches = test("ObjCPtr/isKindOfClassReturnsTrueForCorrectClass") = []
{
    @autoreleasepool
    {
        OPtr<NSString> s([[NSString alloc] initWithUTF8String:"x"]);
        check(s.template isKindOfClass<NSString>());
    }
};

auto tIsKindOfClassMismatch = test("ObjCPtr/isKindOfClassReturnsFalseForWrongClass") = []
{
    @autoreleasepool
    {
        OPtr<NSString> s([[NSString alloc] initWithUTF8String:"x"]);
        check(!s.template isKindOfClass<NSDictionary>());
    }
};

auto tAllocReturnsNonNil = test("ObjCPtr/allocReturnsNonNil") = []
{
    counted::reset();
    auto* obj = alloc<Counted>();
    check(obj != nil);
    [obj init];
    [obj release];
    check(counted::alive.load() == 0);
};

auto tCreateNewIsPlusOne = test("ObjCPtr/createNewReturnsRetainedOne") = []
{
    counted::reset();
    auto* obj = createNew<Counted>();
    check(obj.retainCount == 1);
    check(counted::alive.load() == 1);
    [obj release];
    check(counted::alive.load() == 0);
};

auto tAttachPtrRetains = test("ObjCPtr/attachPtrRetainsInput") = []
{
    counted::reset();
    auto* obj = [[Counted alloc] init];
    {
        OPtr<Counted> p = attachPtr(obj);
        check(obj.retainCount == 2);
    }
    check(obj.retainCount == 1);
    [obj release];
    check(counted::alive.load() == 0);
};

auto tMakePtrDoesNotLeak = test("ObjCPtr/makePtrDoesNotLeak") = []
{
    counted::reset();
    {
        OPtr<Counted> p = makePtr<Counted>();
        check(p.get() != nullptr);
        check(counted::alive.load() == 1);
    }
    check(counted::alive.load() == 0);
};
