// SPDX-License-Identifier: MPL-2.0
#import <Foundation/Foundation.h>
#import <dlfcn.h>

static void RecordBootstrap(NSString *message) {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/jit-helper-bootstrap.txt"];
    [message writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
static void RecordException(NSException *exception) {
    RecordBootstrap([NSString stringWithFormat:@"Bootstrap exception: %@: %@", exception.name, exception.reason]);
}
extern "C" __attribute__((used, visibility("default")))
int NSExtensionMain(int argc, char **argv) {
    @autoreleasepool {
        RecordBootstrap(@"Entered extension main");
        NSSetUncaughtExceptionHandler(RecordException);
    }
    auto original = reinterpret_cast<int (*)(int, char **)>(dlsym(RTLD_NEXT, "NSExtensionMain"));
    return original ? original(argc, argv) : 1;
}
