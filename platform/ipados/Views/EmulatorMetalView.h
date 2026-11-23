#pragma once

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

/// UIView subclass whose backing layer is a CAMetalLayer.
/// This view is the rendering surface for the Cemu Metal renderer.
@interface EmulatorMetalView : UIView

/// The backing CAMetalLayer (readonly convenience accessor).
@property (nonatomic, readonly, nonnull) CAMetalLayer *metalLayer;

/// Selects the existing Cemu GamePad output path instead of the TV output path.
@property (nonatomic) BOOL rendersDRC;
@property (nonatomic) BOOL nativeResolution;
/// Keeps the renderer surface registered while this exact view moves between windows.
@property (nonatomic) BOOL transferringBetweenWindows;

/// Callback invoked when the view is attached to or detached from a UIWindow.
@property (nonatomic, copy, nullable) void (^onLayerAttachedChange)(BOOL attached);

@end

/// UI-owned debug surface that samples the existing DRC source texture.
/// It is intentionally separate from Cemu's TV and pad CAMetalLayers.
@interface DRCPreviewMetalView : UIView

@property (nonatomic) BOOL previewActive;
@property (nonatomic) BOOL nativeResolution;

/// Draw the last valid bridge-owned DRC texture once, if available.
- (void)displayLatestTexture;

@end
