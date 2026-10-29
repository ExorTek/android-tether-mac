/*
 * main.m -- the app whose only job is to carry the driver.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * macOS will not install a DriverKit extension on its own: an
 * application has to ask, with OSSystemExtensionRequest, and that
 * application has to stay installed. Delete it and the system
 * deactivates the extension it carried. That is the whole reason this
 * target exists, and it is why the window says so.
 *
 * Deliberately AppKit and deliberately small: a driver installer that
 * pulls in a UI framework stack is a larger thing to audit than the
 * driver it installs.
 */

#import <AppKit/AppKit.h>
#import <SystemExtensions/SystemExtensions.h>

static NSString *const kDriverBundleID = @"com.exortek.androidtether.driver";

@interface TetherInstaller : NSObject <NSApplicationDelegate,
                                       OSSystemExtensionRequestDelegate>
@property (nonatomic, strong) NSWindow   *window;
@property (nonatomic, strong) NSTextField *status;
@property (nonatomic, strong) NSButton    *installButton;
@end

@implementation TetherInstaller

- (void)showStatus:(NSString *)text
{
    dispatch_async(dispatch_get_main_queue(), ^{
        self.status.stringValue = text;
    });
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    const NSRect frame = NSMakeRect(0, 0, 480, 230);

    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"Android Tether";
    [self.window center];

    NSTextField *title = [NSTextField labelWithString:@"Android USB tethering"];
    title.font = [NSFont boldSystemFontOfSize:15];
    title.frame = NSMakeRect(20, 180, 440, 24);

    NSTextField *blurb = [NSTextField wrappingLabelWithString:
        @"Installs a network driver for USB tethering from an Android phone. "
        @"Keep this app in your Applications folder: deleting it removes the "
        @"driver."];
    blurb.frame = NSMakeRect(20, 126, 440, 50);
    blurb.textColor = [NSColor secondaryLabelColor];

    self.installButton = [NSButton buttonWithTitle:@"Install Driver"
                                            target:self
                                            action:@selector(install:)];
    self.installButton.frame = NSMakeRect(20, 84, 140, 32);
    self.installButton.keyEquivalent = @"\r";

    /*
     * Wrapping, and three lines tall. The failures worth reading are the
     * long ones -- "Missing entitlement
     * com.apple.developer.system-extension.install" is the whole
     * diagnosis, and a single-line label truncates it to nothing useful
     * right when someone needs it.
     */
    self.status = [NSTextField wrappingLabelWithString:@"Not installed."];
    self.status.frame = NSMakeRect(20, 14, 440, 56);
    self.status.textColor = [NSColor secondaryLabelColor];
    self.status.font = [NSFont monospacedDigitSystemFontOfSize:11
                                                        weight:NSFontWeightRegular];
    self.status.selectable = YES;   /* so the message can be copied */

    NSView *content = self.window.contentView;
    [content addSubview:title];
    [content addSubview:blurb];
    [content addSubview:self.installButton];
    [content addSubview:self.status];

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    return YES;
}

- (void)install:(id)sender
{
    [self showStatus:@"Requesting activation..."];

    OSSystemExtensionRequest *request = [OSSystemExtensionRequest
        activationRequestForExtension:kDriverBundleID
                                queue:dispatch_get_main_queue()];
    request.delegate = self;
    [[OSSystemExtensionManager sharedManager] submitRequest:request];
}

#pragma mark - OSSystemExtensionRequestDelegate

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
    [self showStatus:@"Waiting for approval in System Settings > "
                    @"Privacy & Security."];
}

- (void)request:(OSSystemExtensionRequest *)request
    didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        [self showStatus:@"Installed. Restart to finish."];
        return;
    }
    [self showStatus:@"Installed. Connect your phone and turn on USB tethering."];
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    [self showStatus:[NSString stringWithFormat:@"Failed: %@",
                                               error.localizedDescription]];
}

@end

int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        TetherInstaller *delegate = [[TetherInstaller alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
