/*
 * main.m -- the app: tethering today, and the driver when it can be had.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * Two things live in this window, and they are kept visibly apart.
 *
 * What you see first works now. It runs android-tether, the user-space
 * bridge that ships inside this bundle, as root -- macOS asks for an
 * administrator's password each time, because nothing short of a signed
 * privileged helper can avoid that -- and shows how it is doing.
 *
 * Under Details is the reason this target existed first. macOS will not
 * install a DriverKit extension on its own: an application has to ask,
 * with OSSystemExtensionRequest, and that application has to stay
 * installed. Delete it and the system deactivates the extension it
 * carried. It is under Details because, until Apple grants the
 * entitlements, its button can only fail.
 *
 * Deliberately AppKit and deliberately one file: a thing that asks for
 * the administrator's password should be a short read.
 */

#import <AppKit/AppKit.h>
#import <SystemExtensions/SystemExtensions.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static NSString *const kDriverBundleID = @"com.exortek.androidtether.driver";
static NSString *const kHelperName = @"android-tether";

#define HELPER_START_TIMEOUT_S  20      /* from the password to the first word */
#define LOG_MAX_CHARS           200000
#define CONTENT_WIDTH           480.0
#define MARGIN                  22.0

static NSString *const kIdleHint =
    @"Plug the phone in and turn on USB tethering on it, then click Connect. "
    @"macOS asks for an administrator's password, because creating a network "
    @"interface needs one.";

typedef NS_ENUM(NSInteger, TetherState) {
    TetherIdle,         /* nothing running */
    TetherStarting,     /* asked for the password; waiting for the helper */
    TetherHandshake,    /* the helper is talking to the phone */
    TetherConnected,    /* traffic is going through the phone */
    TetherStopping      /* asked to stop; waiting for it to tidy up */
};

/* A string as an AppleScript literal. */
static NSString *
appleScriptLiteral(NSString *s)
{
    s = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    s = [s stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    return [NSString stringWithFormat:@"\"%@\"", s];
}

static NSString *
formatRate(double bitsPerSecond)
{
    if (bitsPerSecond >= 1e6)
        return [NSString stringWithFormat:@"%.1f Mbit/s", bitsPerSecond / 1e6];
    if (bitsPerSecond >= 1e3)
        return [NSString stringWithFormat:@"%.0f kbit/s", bitsPerSecond / 1e3];
    return @"0 kbit/s";
}

static NSString *
formatBytes(unsigned long long bytes)
{
    if (bytes >= 1000000000ull)
        return [NSString stringWithFormat:@"%.2f GB", bytes / 1e9];
    if (bytes >= 1000000ull)
        return [NSString stringWithFormat:@"%.1f MB", bytes / 1e6];
    return [NSString stringWithFormat:@"%llu kB", bytes / 1000];
}

/*
 * The failures a person can do something about, in words that say what.
 * Anything else is shown as the bridge put it, which is the right thing
 * to paste into a bug report.
 */
static NSString *
explainFailure(NSString *failure)
{
    if ([failure containsString:@"no RNDIS control interface"])
        return @"No phone found. Plug it in, turn on USB tethering on it, and try again.";
    if ([failure hasPrefix:@"opening the "])
        return @"Something else is using the phone's tethering interface. Is android-tether "
               @"already running in a terminal?";
    if ([failure containsString:@"could not lease an address"])
        return @"The phone did not hand out an address. Turn USB tethering off and on again.";
    return [NSString stringWithFormat:@"%@. Details has the full log.", failure];
}

@interface TetherApp : NSObject <NSApplicationDelegate,
                                 OSSystemExtensionRequestDelegate>
@property (nonatomic, strong) NSWindow    *window;
@property (nonatomic, strong) NSStackView *root;

/* tethering */
@property (nonatomic, strong) NSView      *dot;
@property (nonatomic, strong) NSTextField *stateLabel;
@property (nonatomic, strong) NSTextField *detailLabel;
@property (nonatomic, strong) NSButton    *connectButton;
@property (nonatomic, strong) NSTextField *downRate, *downTotal, *upRate, *upTotal;
@property (nonatomic, strong) NSButton    *detailsToggle;
@property (nonatomic, strong) NSStackView *details;
@property (nonatomic, strong) NSTextView  *logView;

@property (nonatomic) TetherState state;
@property (nonatomic, copy) NSString *socketPath;
@property (nonatomic) int listenFd;
@property (nonatomic) int helperFd;
@property (nonatomic, strong) dispatch_source_t acceptSource;
@property (nonatomic, strong) dispatch_source_t readSource;
@property (nonatomic, strong) NSMutableData *pending;      /* a line not yet ended */
@property (nonatomic, copy) NSString *lastFailure;
@property (nonatomic, copy) NSString *lastReason;
@property (nonatomic) NSUInteger attempt;                  /* which Connect a timer belongs to */

@property (nonatomic, copy) NSString *linkSummary;         /* "utun4 · 10.0.0.2" */
@property (nonatomic, strong) NSDate *connectedAt;
@property (nonatomic, strong) NSDate *lastSample;
@property (nonatomic) unsigned long long lastRx, lastTx;

/* the driver */
@property (nonatomic, strong) NSTextField *driverStatus;
@property (nonatomic, strong) NSButton    *installButton;
@end

@implementation TetherApp

#pragma mark - building the window

/* No nib, so no menu unless one is made: without it there is no Cmd-Q,
 * and no Cmd-C for the log this window exists to let people copy. */
- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
    appItem.submenu = [[NSMenu alloc] init];
    [appItem.submenu addItemWithTitle:@"About Android Tether"
                               action:@selector(orderFrontStandardAboutPanel:)
                        keyEquivalent:@""];
    [appItem.submenu addItem:[NSMenuItem separatorItem]];
    [appItem.submenu addItemWithTitle:@"Hide Android Tether" action:@selector(hide:)
                        keyEquivalent:@"h"];
    [appItem.submenu addItem:[NSMenuItem separatorItem]];
    [appItem.submenu addItemWithTitle:@"Quit Android Tether" action:@selector(terminate:)
                        keyEquivalent:@"q"];

    NSMenuItem *editItem = [bar addItemWithTitle:@"Edit" action:NULL keyEquivalent:@""];
    editItem.submenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editItem.submenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editItem.submenu addItemWithTitle:@"Select All" action:@selector(selectAll:)
                         keyEquivalent:@"a"];

    NSMenuItem *windowItem = [bar addItemWithTitle:@"Window" action:NULL keyEquivalent:@""];
    windowItem.submenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowItem.submenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:)
                           keyEquivalent:@"m"];
    [windowItem.submenu addItemWithTitle:@"Close" action:@selector(performClose:)
                           keyEquivalent:@"w"];

    NSApp.mainMenu = bar;
}

- (NSTextField *)text:(NSString *)string size:(CGFloat)size weight:(NSFontWeight)weight
            secondary:(BOOL)secondary
{
    NSTextField *field = [NSTextField labelWithString:string];
    field.font = [NSFont systemFontOfSize:size weight:weight];
    if (secondary)
        field.textColor = [NSColor secondaryLabelColor];
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
}

- (NSTextField *)paragraph:(NSString *)string
{
    NSTextField *field = [NSTextField wrappingLabelWithString:string];
    field.font = [NSFont systemFontOfSize:12];
    field.textColor = [NSColor secondaryLabelColor];
    field.preferredMaxLayoutWidth = CONTENT_WIDTH;
    [field.widthAnchor constraintEqualToConstant:CONTENT_WIDTH].active = YES;
    return field;
}

/* One of the two panels: a caption, a rate in large figures, a total. */
- (NSView *)tile:(NSString *)caption rate:(NSTextField *__strong *)rate
           total:(NSTextField *__strong *)total
{
    NSBox *box = [[NSBox alloc] init];
    box.boxType = NSBoxCustom;
    box.titlePosition = NSNoTitle;
    box.cornerRadius = 10;
    box.borderWidth = 1;
    box.borderColor = [NSColor separatorColor];
    box.fillColor = [NSColor controlBackgroundColor];
    box.contentViewMargins = NSZeroSize;

    NSTextField *captionLabel = [self text:caption size:11 weight:NSFontWeightMedium
                                 secondary:YES];
    *rate = [self text:@"--" size:22 weight:NSFontWeightSemibold secondary:NO];
    (*rate).font = [NSFont monospacedDigitSystemFontOfSize:22 weight:NSFontWeightSemibold];
    *total = [self text:@" " size:11 weight:NSFontWeightRegular secondary:YES];
    (*total).font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];

    NSStackView *stack = [NSStackView stackViewWithViews:@[ captionLabel, *rate, *total ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 3;
    stack.edgeInsets = NSEdgeInsetsMake(12, 14, 12, 14);
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *content = box.contentView;
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
    ]];
    return box;
}

- (NSView *)buildHeader
{
    NSImageView *icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [icon.widthAnchor constraintEqualToConstant:56].active = YES;
    [icon.heightAnchor constraintEqualToConstant:56].active = YES;

    NSStackView *words = [NSStackView stackViewWithViews:@[
        [self text:@"Android Tether" size:20 weight:NSFontWeightBold secondary:NO],
        [self text:@"Internet through your phone, over USB" size:13
            weight:NSFontWeightRegular secondary:YES],
    ]];
    words.orientation = NSUserInterfaceLayoutOrientationVertical;
    words.alignment = NSLayoutAttributeLeading;
    words.spacing = 2;

    NSStackView *header = [NSStackView stackViewWithViews:@[ icon, words ]];
    header.spacing = 14;
    header.alignment = NSLayoutAttributeCenterY;
    return header;
}

- (NSView *)buildStatusRow
{
    self.dot = [[NSView alloc] init];
    self.dot.wantsLayer = YES;
    self.dot.layer.cornerRadius = 5;
    [self.dot.widthAnchor constraintEqualToConstant:10].active = YES;
    [self.dot.heightAnchor constraintEqualToConstant:10].active = YES;

    self.stateLabel = [self text:@"Not connected" size:15 weight:NSFontWeightSemibold
                       secondary:NO];
    [self.stateLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                              forOrientation:NSLayoutConstraintOrientationHorizontal];

    self.connectButton = [NSButton buttonWithTitle:@"Connect"
                                            target:self
                                            action:@selector(toggleConnection:)];
    self.connectButton.controlSize = NSControlSizeLarge;
    self.connectButton.keyEquivalent = @"\r";
    [self.connectButton.widthAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;

    NSView *spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *row = [NSStackView stackViewWithViews:@[
        self.dot, self.stateLabel, spacer, self.connectButton
    ]];
    row.spacing = 8;
    row.alignment = NSLayoutAttributeCenterY;
    [row.widthAnchor constraintEqualToConstant:CONTENT_WIDTH].active = YES;
    return row;
}

- (NSView *)buildTiles
{
    NSTextField *downRate, *downTotal, *upRate, *upTotal;
    NSView *down = [self tile:@"DOWNLOAD" rate:&downRate total:&downTotal];
    NSView *up = [self tile:@"UPLOAD" rate:&upRate total:&upTotal];
    self.downRate = downRate;
    self.downTotal = downTotal;
    self.upRate = upRate;
    self.upTotal = upTotal;

    NSStackView *tiles = [NSStackView stackViewWithViews:@[ down, up ]];
    tiles.spacing = 12;
    tiles.distribution = NSStackViewDistributionFillEqually;
    [tiles.widthAnchor constraintEqualToConstant:CONTENT_WIDTH].active = YES;
    return tiles;
}

- (NSView *)buildDetailsToggle
{
    self.detailsToggle = [[NSButton alloc] init];
    self.detailsToggle.bezelStyle = NSBezelStyleDisclosure;
    self.detailsToggle.buttonType = NSButtonTypePushOnPushOff;
    self.detailsToggle.title = @"";
    self.detailsToggle.target = self;
    self.detailsToggle.action = @selector(toggleDetails:);

    NSStackView *row = [NSStackView stackViewWithViews:@[
        self.detailsToggle,
        [self text:@"Details" size:12 weight:NSFontWeightMedium secondary:YES],
    ]];
    row.spacing = 4;
    return row;
}

- (NSStackView *)buildDetails
{
    /*
     * Everything the bridge says, word for word and selectable. When this
     * does not work the useful report is these lines, and a window that
     * boiled them down to "Failed" would be hiding the diagnosis.
     */
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSLineBorder;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll.widthAnchor constraintEqualToConstant:CONTENT_WIDTH].active = YES;
    [scroll.heightAnchor constraintEqualToConstant:150].active = YES;

    self.logView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, CONTENT_WIDTH, 150)];
    self.logView.editable = NO;
    self.logView.selectable = YES;
    self.logView.verticallyResizable = YES;
    self.logView.autoresizingMask = NSViewWidthSizable;
    self.logView.textContainerInset = NSMakeSize(4, 5);
    self.logView.font = [NSFont monospacedSystemFontOfSize:9.5 weight:NSFontWeightRegular];
    scroll.documentView = self.logView;

    NSBox *rule = [[NSBox alloc] init];
    rule.boxType = NSBoxSeparator;
    [rule.widthAnchor constraintEqualToConstant:CONTENT_WIDTH].active = YES;

    self.installButton = [NSButton buttonWithTitle:@"Install Driver"
                                            target:self
                                            action:@selector(install:)];

    /*
     * Wrapping, not one line. The failures worth reading are the long
     * ones -- "Missing entitlement
     * com.apple.developer.system-extension.install" is the whole
     * diagnosis, and a single-line label truncates it to nothing useful
     * right when someone needs it.
     */
    self.driverStatus = [self paragraph:@"Not installed."];
    self.driverStatus.selectable = YES;     /* so the message can be copied */

    NSStackView *details = [NSStackView stackViewWithViews:@[
        scroll,
        rule,
        [self text:@"The driver (not available yet)" size:13 weight:NSFontWeightSemibold
            secondary:NO],
        [self paragraph:
            @"The permanent way: the phone as an ordinary Ethernet interface, with no "
            @"password and nothing to click. It cannot be installed until Apple grants "
            @"this project its DriverKit entitlements. If you do install it, keep this "
            @"app in Applications: deleting it removes the driver."],
        self.installButton,
        self.driverStatus,
    ]];
    details.orientation = NSUserInterfaceLayoutOrientationVertical;
    details.alignment = NSLayoutAttributeLeading;
    details.spacing = 10;
    [details setCustomSpacing:14 afterView:scroll];
    return details;
}

/* The window is exactly as tall as what is in it, and grows downwards. */
- (void)fitWindowAnimated:(BOOL)animated
{
    [self.root layoutSubtreeIfNeeded];
    const NSSize size = self.root.fittingSize;
    NSRect frame = [self.window frameRectForContentRect:NSMakeRect(0, 0, size.width, size.height)];
    const NSRect current = self.window.frame;
    frame.origin.x = current.origin.x;
    frame.origin.y = NSMaxY(current) - frame.size.height;
    [self.window setFrame:frame display:YES animate:animated];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    self.listenFd = -1;
    self.helperFd = -1;
    [self buildMenu];

    self.window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, CONTENT_WIDTH + 2 * MARGIN, 300)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"Android Tether";

    self.detailLabel = [self paragraph:kIdleHint];
    self.details = [self buildDetails];
    self.details.hidden = YES;

    NSView *statusRow = [self buildStatusRow];
    NSView *tiles = [self buildTiles];
    self.root = [NSStackView stackViewWithViews:@[
        [self buildHeader], statusRow, self.detailLabel, tiles,
        [self buildDetailsToggle], self.details,
    ]];
    self.root.orientation = NSUserInterfaceLayoutOrientationVertical;
    self.root.alignment = NSLayoutAttributeLeading;
    self.root.spacing = 14;
    self.root.edgeInsets = NSEdgeInsetsMake(MARGIN, MARGIN, MARGIN, MARGIN);
    [self.root setCustomSpacing:20 afterView:self.root.views.firstObject];
    [self.root setCustomSpacing:8 afterView:statusRow];
    self.root.translatesAutoresizingMaskIntoConstraints = NO;

    /* Pinned at the top only: the window is sized to the stack, not the
     * stack stretched to the window. */
    NSView *content = self.window.contentView;
    [content addSubview:self.root];
    [NSLayoutConstraint activateConstraints:@[
        [self.root.topAnchor constraintEqualToAnchor:content.topAnchor],
        [self.root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [self.root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
    ]];

    self.state = TetherIdle;
    [self fitWindowAnimated:NO];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    return YES;
}

/*
 * Quitting is disconnecting. Closing our end is all it takes: the helper
 * reads end-of-file and puts the network back, and it would do the same
 * had this process been killed instead.
 */
- (void)applicationWillTerminate:(NSNotification *)note
{
    [self dropListener];
    if (self.helperFd >= 0)
        close(self.helperFd);
}

- (void)toggleDetails:(id)sender
{
    self.details.hidden = self.detailsToggle.state != NSControlStateValueOn;
    [self fitWindowAnimated:YES];
}

#pragma mark - tethering: state

- (void)setDotColor:(NSColor *)color
{
    /* Resolved against the window's appearance, or a dark-mode colour
     * ends up on a light window and the other way about. */
    [self.window.effectiveAppearance performAsCurrentDrawingAppearance:^{
        self.dot.layer.backgroundColor = color.CGColor;
    }];
}

- (void)setState:(TetherState)state
{
    _state = state;
    switch (state) {
    case TetherIdle:
        self.connectButton.title = @"Connect";
        self.connectButton.enabled = YES;
        [self setDotColor:[NSColor tertiaryLabelColor]];
        break;
    case TetherStarting:
    case TetherStopping:
        self.connectButton.enabled = NO;
        [self setDotColor:[NSColor systemOrangeColor]];
        break;
    case TetherHandshake:
        self.connectButton.title = @"Disconnect";
        self.connectButton.enabled = YES;
        [self setDotColor:[NSColor systemOrangeColor]];
        break;
    case TetherConnected:
        self.connectButton.title = @"Disconnect";
        self.connectButton.enabled = YES;
        [self setDotColor:[NSColor systemGreenColor]];
        break;
    }
}

/* The headline, and the sentence under it. */
- (void)show:(NSString *)headline detail:(NSString *)detail
{
    self.stateLabel.stringValue = headline;
    if (![self.detailLabel.stringValue isEqualToString:detail]) {
        self.detailLabel.stringValue = detail;
        [self fitWindowAnimated:NO];        /* it may wrap to more or fewer lines */
    }
}

- (void)clearFigures
{
    self.downRate.stringValue = @"--";
    self.upRate.stringValue = @"--";
    self.downTotal.stringValue = @" ";
    self.upTotal.stringValue = @" ";
}

- (void)appendLog:(NSString *)text
{
    NSTextStorage *storage = self.logView.textStorage;
    NSDictionary *attributes = @{
        NSFontAttributeName : self.logView.font,
        NSForegroundColorAttributeName : [NSColor textColor],
    };
    [storage appendAttributedString:[[NSAttributedString alloc] initWithString:text
                                                                    attributes:attributes]];
    if (storage.length > LOG_MAX_CHARS)
        [storage deleteCharactersInRange:NSMakeRange(0, storage.length - LOG_MAX_CHARS / 2)];
    [self.logView scrollRangeToVisible:NSMakeRange(storage.length, 0)];
}

- (void)toggleConnection:(id)sender
{
    if (self.state == TetherIdle)
        [self connect];
    else if (self.state == TetherHandshake || self.state == TetherConnected)
        [self disconnect];
}

#pragma mark - tethering: starting the helper

- (void)dropListener
{
    if (self.acceptSource != nil) {
        dispatch_source_cancel(self.acceptSource);
        self.acceptSource = nil;
    }
    if (self.listenFd >= 0) {
        close(self.listenFd);
        self.listenFd = -1;
    }
    if (self.socketPath != nil) {
        unlink(self.socketPath.fileSystemRepresentation);
        self.socketPath = nil;
    }
}

/*
 * The socket the helper will connect back to. It lives in this user's own
 * temporary directory, which nobody else can list, and is unlinked the
 * moment the helper has connected.
 */
- (BOOL)listen
{
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"at-%d.sock", getpid()]];
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path.fileSystemRepresentation) >= sizeof(addr.sun_path))
        return NO;
    strlcpy(addr.sun_path, path.fileSystemRepresentation, sizeof(addr.sun_path));
    unlink(addr.sun_path);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NO;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        chmod(addr.sun_path, 0600) != 0 || listen(fd, 1) != 0) {
        close(fd);
        unlink(addr.sun_path);
        return NO;
    }

    self.listenFd = fd;
    self.socketPath = path;
    self.acceptSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0,
                                               dispatch_get_main_queue());
    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(self.acceptSource, ^{ [weakSelf helperArrived]; });
    dispatch_resume(self.acceptSource);
    return YES;
}

- (void)connect
{
    NSString *helper = [[NSBundle mainBundle] pathForAuxiliaryExecutable:kHelperName];
    if (helper == nil) {
        [self show:@"Cannot connect" detail:@"This copy of the app is missing android-tether."];
        return;
    }
    if (![self listen]) {
        [self show:@"Cannot connect" detail:@"Could not create the control socket."];
        return;
    }

    self.logView.string = @"";
    [self clearFigures];
    self.lastFailure = nil;
    self.lastReason = nil;
    self.linkSummary = nil;
    self.connectedAt = nil;
    self.lastSample = nil;
    self.pending = [NSMutableData data];
    self.state = TetherStarting;
    [self show:@"Waiting for your password..."
        detail:@"macOS is asking for an administrator's password."];
    const NSUInteger attempt = ++self.attempt;

    /*
     * `do shell script ... with administrator privileges` is the one way
     * left to run something as root from an app that has no signed
     * privileged helper, and it is the system's own password window that
     * appears. The command is put in the background with its output
     * discarded, so this returns as soon as it has started; everything it
     * has to say comes back over the socket.
     */
    NSString *source = [NSString stringWithFormat:
        @"do shell script (quoted form of %@) & \" --control \" & (quoted form of %@) "
        @"& \" >/dev/null 2>&1 &\" with prompt %@ with administrator privileges",
        appleScriptLiteral(helper), appleScriptLiteral(self.socketPath),
        appleScriptLiteral(@"Android Tether needs an administrator's password to create "
                           @"the network interface that carries your traffic to the phone.")];

    /* After this turn of the run loop, so the window shows the new state
     * before the password window takes the main thread. */
    dispatch_async(dispatch_get_main_queue(), ^{
        NSDictionary *error = nil;
        [[[NSAppleScript alloc] initWithSource:source] executeAndReturnError:&error];
        if (attempt != self.attempt || self.state != TetherStarting)
            return;         /* the helper has already connected, or already gone */

        if (error != nil) {
            [self dropListener];
            self.state = TetherIdle;
            /* -128 is the user pressing Cancel: not a failure to report. */
            if ([error[NSAppleScriptErrorNumber] integerValue] == -128)
                [self show:@"Not connected" detail:kIdleHint];
            else
                [self show:@"Could not start"
                    detail:error[NSAppleScriptErrorMessage] ?: @"Unknown error."];
            return;
        }

        [self show:@"Starting..." detail:@"Waiting for the bridge to report in."];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, HELPER_START_TIMEOUT_S * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            if (attempt != self.attempt || self.state != TetherStarting)
                return;
            [self dropListener];
            self.state = TetherIdle;
            [self setDotColor:[NSColor systemRedColor]];
            [self show:@"Could not start"
                detail:@"android-tether was started but never reported back."];
        });
    });
}

- (void)helperArrived
{
    const int fd = accept(self.listenFd, NULL, NULL);
    if (fd < 0)
        return;
    [self dropListener];            /* one helper; nobody else gets to connect */

    self.helperFd = fd;
    self.state = TetherHandshake;
    [self show:@"Talking to the phone..."
        detail:@"Starting RNDIS and asking the phone for an address."];

    self.readSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0,
                                             dispatch_get_main_queue());
    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(self.readSource, ^{ [weakSelf readHelper]; });
    dispatch_resume(self.readSource);
}

#pragma mark - tethering: what the helper says

- (void)readHelper
{
    char buffer[4096];
    const ssize_t n = read(self.helperFd, buffer, sizeof(buffer));
    if (n <= 0) {
        [self helperGone];
        return;
    }
    [self.pending appendBytes:buffer length:(NSUInteger)n];

    /* Whole lines only; the tail waits for the rest of itself. */
    for (;;) {
        const char *bytes = self.pending.bytes;
        const char *newline = memchr(bytes, '\n', self.pending.length);
        if (newline == NULL)
            break;
        const NSUInteger length = (NSUInteger)(newline - bytes);
        NSString *line = [[NSString alloc] initWithBytes:bytes
                                                  length:length
                                                encoding:NSUTF8StringEncoding] ?: @"";
        [self.pending replaceBytesInRange:NSMakeRange(0, length + 1) withBytes:NULL length:0];
        [self handleLine:line];
    }
}

/*
 * Lines that start with @ are the bridge talking to this app: @up when
 * traffic can flow, @stat with its counters once a second. Everything
 * else was written for a person at a terminal, and goes in the log as it
 * came; of those, only a FAIL and the reason for stopping are read here.
 */
- (void)handleLine:(NSString *)line
{
    if ([line hasPrefix:@"@"]) {
        NSArray<NSString *> *words = [line componentsSeparatedByString:@" "];
        if ([words[0] isEqualToString:@"@up"] && words.count >= 3)
            [self bridgeUp:words[1] address:words[2]];
        else if ([words[0] isEqualToString:@"@stat"] && words.count >= 6)
            [self sample:words];
        return;
    }
    [self appendLog:[line stringByAppendingString:@"\n"]];

    if ([line hasPrefix:@"FAIL  "]) {
        self.lastFailure = [line substringFromIndex:6];
    } else if (self.state == TetherConnected && self.lastReason == nil && line.length > 0 &&
               ![line hasPrefix:@"ok "] && ![line hasPrefix:@" "] &&
               ![line hasPrefix:@"tethering through"]) {
        /* Why it is stopping: "the phone went away", and the like. */
        self.lastReason = line;
    }
}

- (void)bridgeUp:(NSString *)interface address:(NSString *)address
{
    self.linkSummary = [NSString stringWithFormat:@"%@  ·  %@", interface, address];
    self.connectedAt = [NSDate date];
    self.state = TetherConnected;
    [self show:@"Connected through the phone" detail:self.linkSummary];
}

/* @stat rxBytes txBytes rxFrames txFrames errors */
- (void)sample:(NSArray<NSString *> *)words
{
    if (self.state != TetherConnected)
        return;

    const unsigned long long rx = strtoull(words[1].UTF8String, NULL, 10);
    const unsigned long long tx = strtoull(words[2].UTF8String, NULL, 10);
    const unsigned long long errors = strtoull(words[5].UTF8String, NULL, 10);
    NSDate *now = [NSDate date];

    if (self.lastSample != nil) {
        const NSTimeInterval dt = [now timeIntervalSinceDate:self.lastSample];
        if (dt > 0.2 && rx >= self.lastRx && tx >= self.lastTx) {
            self.downRate.stringValue = formatRate((rx - self.lastRx) * 8.0 / dt);
            self.upRate.stringValue = formatRate((tx - self.lastTx) * 8.0 / dt);
        }
    }
    self.lastSample = now;
    self.lastRx = rx;
    self.lastTx = tx;
    self.downTotal.stringValue = [formatBytes(rx) stringByAppendingString:@" received"];
    self.upTotal.stringValue = [formatBytes(tx) stringByAppendingString:@" sent"];

    const long seconds = (long)[now timeIntervalSinceDate:self.connectedAt];
    NSMutableString *detail = [NSMutableString stringWithFormat:@"%@  ·  %ld:%02ld:%02ld",
        self.linkSummary, seconds / 3600, seconds / 60 % 60, seconds % 60];
    if (errors > 0)
        [detail appendFormat:@"  ·  %llu transfer errors", errors];
    [self show:self.stateLabel.stringValue detail:detail];
}

- (void)disconnect
{
    self.state = TetherStopping;
    [self show:@"Disconnecting..." detail:@"Putting the Mac's network back as it was."];
    /* Half-close: it reads end-of-file, and can still tell us how it went. */
    shutdown(self.helperFd, SHUT_WR);
}

- (void)helperGone
{
    const BOOL asked = self.state == TetherStopping;

    dispatch_source_cancel(self.readSource);
    self.readSource = nil;
    close(self.helperFd);
    self.helperFd = -1;
    self.state = TetherIdle;
    self.downRate.stringValue = @"--";
    self.upRate.stringValue = @"--";       /* the totals stay: they are what was used */

    if (asked) {
        [self show:@"Not connected" detail:kIdleHint];
    } else if (self.lastFailure != nil) {
        [self setDotColor:[NSColor systemRedColor]];
        [self show:@"Could not connect" detail:explainFailure(self.lastFailure)];
    } else if (self.lastReason != nil) {
        /* "the phone went away" -> "The phone went away." */
        NSString *reason = self.lastReason;
        reason = [[[reason substringToIndex:1] uppercaseString]
            stringByAppendingString:[reason substringFromIndex:1]];
        [self show:@"Disconnected" detail:[reason stringByAppendingString:@"."]];
    } else {
        [self show:@"Disconnected" detail:@"The bridge stopped. Details has the full log."];
    }
}

#pragma mark - the driver

- (void)showDriverStatus:(NSString *)text
{
    dispatch_async(dispatch_get_main_queue(), ^{
        self.driverStatus.stringValue = text;
        [self fitWindowAnimated:NO];
    });
}

- (void)install:(id)sender
{
    [self showDriverStatus:@"Requesting activation..."];

    OSSystemExtensionRequest *request = [OSSystemExtensionRequest
        activationRequestForExtension:kDriverBundleID
                                queue:dispatch_get_main_queue()];
    request.delegate = self;
    [[OSSystemExtensionManager sharedManager] submitRequest:request];
}

/*
 * Asked when a different build of the same extension is already present.
 * Replacing is the only sensible answer for an installer whose job is to
 * carry exactly one version.
 */
- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                  actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                withExtension:(OSSystemExtensionProperties *)replacement
{
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    [self showDriverStatus:@"Waiting for approval in System Settings > "
                          @"Privacy & Security."];
}

- (void)request:(OSSystemExtensionRequest *)request
    didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        [self showDriverStatus:@"Installed. Restart to finish."];
        return;
    }
    [self showDriverStatus:@"Installed. Connect your phone and turn on USB tethering."];
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    /*
     * The failure nearly everyone will see, and it is not theirs to fix:
     * say so, and point at the part of the window that does work, before
     * giving the system's own words for whoever is chasing it.
     */
    NSString *message = error.localizedDescription;
    if ([message containsString:@"entitlement"]) {
        [self showDriverStatus:[NSString stringWithFormat:
            @"Expected: this build cannot install the driver until Apple grants its "
            @"entitlements. Use Connect, above, instead. (%@)", message]];
        return;
    }
    [self showDriverStatus:[NSString stringWithFormat:@"Failed: %@", message]];
}

@end

int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        TetherApp *delegate = [[TetherApp alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
