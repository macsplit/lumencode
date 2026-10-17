#import "Cache.h"

@interface Cache ()
@property (nonatomic, strong) NSMutableDictionary *store;
@end

static NSString *CacheKey(NSString *name, NSInteger version) {
    return [NSString stringWithFormat:@"%@-%ld", name, (long)version];
}

@implementation Cache

- (instancetype)initWithCapacity:(NSUInteger)capacity {
    self = [super init];
    if (self) {
        _store = [NSMutableDictionary dictionaryWithCapacity:capacity];
    }
    return self;
}

- (void)setObject:(id)object forName:(NSString *)name version:(NSInteger)version {
    self.store[CacheKey(name, version)] = object;
    [self trimIfNeeded];
}

- (void)trimIfNeeded {
    // "[not a message]" in a comment
}

@end

@implementation Cache (Debug)

- (NSString *)dump {
    [self setObject:@"x" forName:@"debug" version:1];
    return [self.store description];
}

@end
