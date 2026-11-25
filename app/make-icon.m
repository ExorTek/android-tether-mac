/*
 * make-icon.m -- draws AppIcon.icns, so the icon has a source.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * A phone, two arrows on its screen, and a cable leaving the bottom of
 * it: the three things this project is. Drawn in code rather than kept
 * as a picture so that it can be reviewed and changed like the rest of
 * the repository, and drawn again at every size rather than scaled, so
 * the 16-pixel one is not a blurred 1024-pixel one.
 *
 *   make -C driver icon        regenerates app/AppIcon.icns
 *
 * The .icns is committed: building the app should not need this to run.
 */

#import <AppKit/AppKit.h>

/* Everything below is laid out on a 1024-unit canvas, y upwards. */
static NSColor *
rgb(int hex)
{
    return [NSColor colorWithSRGBRed:((hex >> 16) & 0xFF) / 255.0
                               green:((hex >> 8) & 0xFF) / 255.0
                                blue:(hex & 0xFF) / 255.0
                               alpha:1.0];
}

static NSBezierPath *
arrow(CGFloat x, CGFloat tail, CGFloat tip)
{
    const CGFloat direction = tip > tail ? 1 : -1;
    const CGFloat shaft = 17, head = 52, headLength = 78;
    const CGFloat neck = tip - direction * headLength;

    NSBezierPath *path = [NSBezierPath bezierPath];
    [path moveToPoint:NSMakePoint(x - shaft, tail)];
    [path lineToPoint:NSMakePoint(x - shaft, neck)];
    [path lineToPoint:NSMakePoint(x - head, neck)];
    [path lineToPoint:NSMakePoint(x, tip)];
    [path lineToPoint:NSMakePoint(x + head, neck)];
    [path lineToPoint:NSMakePoint(x + shaft, neck)];
    [path lineToPoint:NSMakePoint(x + shaft, tail)];
    [path closePath];
    path.lineJoinStyle = NSLineJoinStyleRound;
    path.lineWidth = 14;            /* stroked as well as filled: rounds the corners */
    return path;
}

static void
draw(void)
{
    /* The body: the rounded square every macOS icon sits in. */
    const NSRect body = NSMakeRect(100, 100, 824, 824);
    NSBezierPath *bodyPath = [NSBezierPath bezierPathWithRoundedRect:body
                                                             xRadius:186 yRadius:186];

    [NSGraphicsContext saveGraphicsState];
    NSShadow *shadow = [[NSShadow alloc] init];
    shadow.shadowColor = [NSColor colorWithWhite:0 alpha:0.30];
    shadow.shadowOffset = NSMakeSize(0, -10);
    shadow.shadowBlurRadius = 22;
    [shadow set];
    [rgb(0x0C6E8A) setFill];
    [bodyPath fill];
    [NSGraphicsContext restoreGraphicsState];

    [NSGraphicsContext saveGraphicsState];
    [bodyPath addClip];
    [[[NSGradient alloc] initWithStartingColor:rgb(0x35D6A4) endingColor:rgb(0x0B6384)]
        drawInRect:body angle:-70];

    /* A little light along the top edge. */
    [[[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:1 alpha:0.22]
                                   endingColor:[NSColor colorWithWhite:1 alpha:0.0]]
        drawInRect:NSMakeRect(100, 624, 824, 300) angle:-90];

    [[NSColor whiteColor] set];

    /* The cable, from the phone to the edge of the icon. */
    [[NSBezierPath bezierPathWithRect:NSMakeRect(494, 100, 36, 200)] fill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(462, 246, 100, 60)
                                     xRadius:12 yRadius:12] fill];

    /* The phone: an outline, so the arrows read as being on its screen. */
    NSBezierPath *phone = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(352, 300, 320, 520)
                                                          xRadius:62 yRadius:62];
    [[NSColor colorWithWhite:1 alpha:0.14] setFill];
    [phone fill];
    [[NSColor whiteColor] set];
    phone.lineWidth = 36;
    [phone stroke];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(476, 752, 72, 14)
                                     xRadius:7 yRadius:7] fill];

    /* Up and down: traffic in both directions. */
    NSBezierPath *up = arrow(452, 430, 690);
    NSBezierPath *down = arrow(572, 690, 430);
    [up fill];   [up stroke];
    [down fill]; [down stroke];

    [NSGraphicsContext restoreGraphicsState];
}

static BOOL
render(int pixels, NSString *path)
{
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:pixels pixelsHigh:pixels
                   bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                  colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    rep.size = NSMakeSize(pixels, pixels);

    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    NSAffineTransform *scale = [NSAffineTransform transform];
    [scale scaleBy:pixels / 1024.0];
    [scale concat];
    draw();
    [NSGraphicsContext restoreGraphicsState];

    return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
        writeToFile:path atomically:YES];
}

int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: make-icon OUTPUT.iconset\n");
            return 2;
        }
        NSString *dir = @(argv[1]);
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES attributes:nil error:NULL];

        /* The names iconutil insists on. */
        const int points[] = { 16, 32, 128, 256, 512 };
        for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
            NSString *one = [NSString stringWithFormat:@"%@/icon_%dx%d.png",
                                                       dir, points[i], points[i]];
            NSString *two = [NSString stringWithFormat:@"%@/icon_%dx%d@2x.png",
                                                       dir, points[i], points[i]];
            if (!render(points[i], one) || !render(points[i] * 2, two)) {
                fprintf(stderr, "make-icon: could not write into %s\n", argv[1]);
                return 1;
            }
        }
    }
    return 0;
}
