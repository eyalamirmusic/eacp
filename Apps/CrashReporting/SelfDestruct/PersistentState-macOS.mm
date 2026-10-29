#include "PersistentState.h"

#import <Foundation/Foundation.h>

namespace SelfDestruct
{
void ignorePersistentUIState()
{
    [[NSUserDefaults standardUserDefaults]
        registerDefaults:@{@"ApplePersistenceIgnoreState": @YES}];
}
} // namespace SelfDestruct
