#import <Foundation/Foundation.h>
#include <ReactCommon/CxxTurboModuleUtils.h>

#include "LoomExample.h"

// Registers the C++ TurboModule with React Native's global C++ module map when the library is loaded.
@interface LoomExampleRegistration : NSObject
@end

@implementation LoomExampleRegistration
+ (void)load
{
  facebook::react::registerCxxModuleToGlobalModuleMap("LoomExample", loomexample::createLoomExampleModule);
}
@end
