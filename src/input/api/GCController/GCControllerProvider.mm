#include "Common/precompiled.h"
#include "input/api/GCController/GCControllerProvider.h"
#include "input/api/GCController/GCController.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include "input/InputManager.h"

GCControllerProvider::GCControllerProvider()
{
	auto notifyChanged = ^{
		@autoreleasepool {
			NSArray<GCController*>* controllers = [GCController controllers];
			for (NSUInteger i = 0; i < controllers.count; ++i) {
				if (i < 4) {
					controllers[i].playerIndex = (GCControllerPlayerIndex)i;
				}
			}
		}
		GCController_RefreshCache();
		dispatch_async(dispatch_get_main_queue(), ^{
			InputManager::instance().on_device_changed();
		});
	};

	id connectObs = [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification
																	  object:nil
																	   queue:[NSOperationQueue mainQueue]
																  usingBlock:^(NSNotification * _Nonnull note) {
		notifyChanged();
	}];
	id disconnectObs = [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification
																		 object:nil
																		  queue:[NSOperationQueue mainQueue]
																	 usingBlock:^(NSNotification * _Nonnull note) {
		notifyChanged();
	}];
	id becomeCurrentObs = [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidBecomeCurrentNotification
																			object:nil
																			 queue:[NSOperationQueue mainQueue]
																		usingBlock:^(NSNotification * _Nonnull note) {
		notifyChanged();
	}];
	m_connectObserver = (__bridge_retained void*)connectObs;
	m_disconnectObserver = (__bridge_retained void*)disconnectObs;
	m_becomeCurrentObserver = (__bridge_retained void*)becomeCurrentObs;

	// Initial playerIndex assignment
	@autoreleasepool {
		NSArray<GCController*>* controllers = [GCController controllers];
		for (NSUInteger i = 0; i < controllers.count; ++i) {
			if (i < 4) {
				controllers[i].playerIndex = (GCControllerPlayerIndex)i;
			}
		}
	}
	GCController_RefreshCache();
}

GCControllerProvider::~GCControllerProvider()
{
	if (m_connectObserver) {
		id obs = (__bridge_transfer id)m_connectObserver;
		[[NSNotificationCenter defaultCenter] removeObserver:obs];
		m_connectObserver = nullptr;
	}
	if (m_disconnectObserver) {
		id obs = (__bridge_transfer id)m_disconnectObserver;
		[[NSNotificationCenter defaultCenter] removeObserver:obs];
		m_disconnectObserver = nullptr;
	}
	if (m_becomeCurrentObserver) {
		id obs = (__bridge_transfer id)m_becomeCurrentObserver;
		[[NSNotificationCenter defaultCenter] removeObserver:obs];
		m_becomeCurrentObserver = nullptr;
	}
}

std::vector<std::shared_ptr<ControllerBase>> GCControllerProvider::get_controllers()
{
	std::vector<std::shared_ptr<ControllerBase>> result;
	@autoreleasepool {
		NSArray<GCController*>* controllers = [GCController controllers];
		for (NSUInteger i = 0; i < controllers.count; ++i) {
			GCController* gc = controllers[i];
			NSString* name = gc.vendorName ?: @"Game Controller";
			std::string displayName = [name UTF8String];
			std::string uuid = std::to_string(i);
			result.emplace_back(std::make_shared<AppleGCController>(i, uuid, displayName));
		}
	}
	return result;
}
