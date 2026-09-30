#import <Foundation/Foundation.h>

@interface PriceFormatter : NSObject
+ (NSString *)stringForCents:(NSInteger)cents currency:(NSString *)currency;
@end

@implementation PriceFormatter
+ (NSString *)stringForCents:(NSInteger)cents currency:(NSString *)currency
{
    return [NSString stringWithFormat:@"%@ %ld", currency, (long)cents];
}
@end
