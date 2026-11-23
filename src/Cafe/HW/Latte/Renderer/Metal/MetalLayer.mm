#include "Cafe/HW/Latte/Renderer/Metal/MetalLayer.h"

#if TARGET_OS_IPHONE || TARGET_OS_IOS
#import <QuartzCore/CAMetalLayer.h>

void* CreateMetalLayer(void* handle, float& scaleX, float& scaleY)
{
	CAMetalLayer* layer = (__bridge CAMetalLayer*)handle;
	if (layer)
	{
		scaleX = 1.0f;
		scaleY = 1.0f;
		return (__bridge_retained void*)layer;
	}
	scaleX = 1.0f;
	scaleY = 1.0f;
	return nullptr;
}
#else
#include "Cafe/HW/Latte/Renderer/MetalView.h"

void* CreateMetalLayer(void* handle, float& scaleX, float& scaleY)
{
	NSView* view = (NSView*)handle;

	MetalView* childView = [[MetalView alloc] initWithFrame:view.bounds];
	childView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	childView.wantsLayer = YES;

	[view addSubview:childView];

	const NSRect points = [childView frame];
    const NSRect pixels = [childView convertRectToBacking:points];

	scaleX = (float)(pixels.size.width / points.size.width);
    scaleY = (float)(pixels.size.height / points.size.height);

	return childView.layer;
}
#endif
