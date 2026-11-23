// SPDX-License-Identifier: MPL-2.0
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT BOOL CemuJITHasGetTaskAllow(void);

// Starts the bundled helper only for a user-initiated JIT operation. Gameplay
// does not call through this object once executable memory has been prepared.
@interface CemuJITLauncher : NSObject
- (void)startWithPort:(uint16_t)port
                token:(NSString *)token
           completion:(void (^)(NSError * _Nullable error))completion
    NS_SWIFT_NAME(start(port:token:completion:));
- (void)invalidate;
@end

NS_ASSUME_NONNULL_END
