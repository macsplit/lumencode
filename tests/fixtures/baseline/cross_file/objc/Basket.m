#import <Foundation/Foundation.h>

@interface Basket : NSObject
- (NSString *)totalLabel;
@end

@implementation Basket
- (NSString *)totalLabel
{
    return [PriceFormatter stringForCents:1250 currency:@"EUR"];
}
@end
