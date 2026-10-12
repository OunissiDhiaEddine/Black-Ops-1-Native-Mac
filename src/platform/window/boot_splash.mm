// Boot credit (P8 / T-065). Plain Core Animation layers on top of the game's Metal layer: nothing here touches
// the renderer, and the game draws underneath as usual.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdlib>
#include <cstring>

#include "boot_splash.h"

namespace bo1::win {

const char *const kBootSplashText = "Ported with love to the Mac by Didou Archer";

// A black layer of the given size with the line centred in it; both follow a resize of their parent.
static CALayer *make_splash_layer(CGSize size, CGFloat scale) {
    CALayer *black = [CALayer layer];
    black.frame = CGRectMake(0, 0, size.width, size.height);
    black.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    black.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    black.zPosition = 1000;

    CGFloat font_size = size.height / 24.0;
    NSFont *font = [NSFont systemFontOfSize:font_size weight:NSFontWeightLight];
    NSString *text = [NSString stringWithUTF8String:kBootSplashText];
    CGSize text_size = [text sizeWithAttributes:@{NSFontAttributeName : font}];
    CATextLayer *line = [CATextLayer layer];
    line.string = text;
    line.font = (__bridge CFTypeRef)font;
    line.fontSize = font_size;
    line.foregroundColor = CGColorGetConstantColor(kCGColorWhite);
    line.alignmentMode = kCAAlignmentCenter;
    line.contentsScale = scale;
    CGFloat w = ceil(text_size.width) + 8, h = ceil(text_size.height) + 4;
    line.frame = CGRectMake(floor((size.width - w) / 2), floor((size.height - h) / 2), w, h);
    line.autoresizingMask = kCALayerMinXMargin | kCALayerMaxXMargin | kCALayerMinYMargin | kCALayerMaxYMargin;
    [black addSublayer:line];
    return black;
}

bool show_boot_splash(void *metal_layer, double seconds) {
    const char *sw = getenv("BO1_SPLASH");
    if (!metal_layer || (sw && strcmp(sw, "off") == 0)) return false;
    CALayer *parent = (__bridge CALayer *)metal_layer;
    dispatch_async(dispatch_get_main_queue(), ^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        CALayer *splash = make_splash_layer(parent.bounds.size, parent.contentsScale);
        [parent addSublayer:splash];
        [CATransaction commit];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [CATransaction begin];
            [CATransaction setAnimationDuration:0.3];
            [CATransaction setCompletionBlock:^{ [splash removeFromSuperlayer]; }];
            splash.opacity = 0;
            [CATransaction commit];
        });
    });
    return true;
}

bool write_boot_splash_png(const char *path, int width, int height) {
    @autoreleasepool {
        CALayer *splash = make_splash_layer(CGSizeMake(width, height), 1);
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(nullptr, (size_t)width, (size_t)height, 8, 0, rgb, kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(rgb);
        if (!ctx) return false;
        [splash renderInContext:ctx];
        CGImageRef image = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
        CGImageRelease(image);
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
    }
}

} // namespace bo1::win
