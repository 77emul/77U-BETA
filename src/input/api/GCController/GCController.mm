#include "input/api/GCController/GCController.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

namespace
{
	std::atomic<uint64> s_motionRecenterGeneration{0};
	std::atomic<bool> s_motionEnabled{true};
    std::atomic<bool> s_menuCaptured{false};
    std::atomic<bool> s_touchEnabled{false};
    std::atomic<bool> s_touchMotionEnabled{false};
    std::mutex s_touchMutex;
    struct TouchState { uint64 buttons = 0; glm::vec2 left{}, right{}; };
    TouchState s_touch;
    // CMDeviceMotion.rotationRate is already unbiased. Learning a second bias
    // mistakes gentle intentional turns for drift and creates motion at rest.
    AppleMotionHandler s_touchMotion{GyroBiasMode::AlreadyCorrected};
    uint64 s_touchMotionGeneration = 0;
    bool s_touchMotionPortrait = false;
    std::chrono::steady_clock::time_point s_touchMotionTime{};

	GCController* GetController(size_t index)
	{
		if (index >= 4) return nil;
		@autoreleasepool {
			GCController* target = nil;

			// For Player 1 (index 0), prefer the active / current controller
			if (index == 0) {
				GCController* cur = [GCController current];
				if (cur && (cur.extendedGamepad != nil || cur.microGamepad != nil || cur.gamepad != nil)) {
					target = cur;
				}
			}

			// If not found yet, search [GCController controllers]
			if (!target) {
				NSArray<GCController*>* controllers = [GCController controllers];

				// 1. Try matching by explicit playerIndex
				for (GCController* c in controllers) {
					if (c.playerIndex == (GCControllerPlayerIndex)index &&
						(c.extendedGamepad != nil || c.microGamepad != nil || c.gamepad != nil)) {
						target = c;
						break;
					}
				}

				// 2. Otherwise pick the N-th available real gamepad
				if (!target) {
					size_t realPadIndex = 0;
					for (GCController* c in controllers) {
						if (c.extendedGamepad != nil || c.microGamepad != nil || c.gamepad != nil) {
							if (realPadIndex == index) {
								target = c;
								break;
							}
							realPadIndex++;
						}
					}
				}

				// 3. Fallback for Player 1: first available controller
				if (!target && index == 0 && controllers.count > 0) {
					target = controllers.firstObject;
				}
			}

			return target;
		}
	}
}

void GCController_SetMenuCaptured(bool captured)
{
    s_menuCaptured.store(captured, std::memory_order_relaxed);
}

void GCController_RefreshCache()
{
	@autoreleasepool {
		GCController* c = [GCController current] ?: [GCController controllers].firstObject;
		if (c) {
			NSString* name = c.vendorName ?: @"Game Controller";
			cemuLog_log(LogType::Force, "[GCController] Active controller: '{}' (category: {})",
				[name UTF8String], [(c.productCategory ?: @"Unknown") UTF8String]);
		} else {
			cemuLog_log(LogType::Force, "[GCController] No physical controller attached");
		}
	}
}

void GCController_RequestMotionRecenter()
{
	s_motionRecenterGeneration.fetch_add(1, std::memory_order_release);
}

void GCController_SetMotionEnabled(bool enabled)
{
	s_motionEnabled.store(enabled, std::memory_order_release);
}

bool GCController_IsMotionEnabled()
{
	return s_motionEnabled.load(std::memory_order_acquire);
}

void GCController_SetTouchEnabled(bool enabled)
{
    std::scoped_lock lock(s_touchMutex);
    s_touch = {};
    s_touchEnabled.store(enabled, std::memory_order_release);
}

void GCController_UpdateTouch(uint64 buttons, float lx, float ly, float rx, float ry)
{
    auto normalized = [](float value) { return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f; };
    std::scoped_lock lock(s_touchMutex);
    if (!s_touchEnabled.load(std::memory_order_acquire)) { s_touch = {}; return; }
    s_touch.buttons = buttons & (0x7FFFULL | (1ULL << kButtonZL) | (1ULL << kButtonZR));
    s_touch.left = {normalized(lx), normalized(ly)};
    s_touch.right = {normalized(rx), normalized(ry)};
}

bool GCController_GetTouchSnapshot(GCControllerTouchSnapshot& snapshot)
{
    std::scoped_lock lock(s_touchMutex);
    if (!s_touchEnabled.load(std::memory_order_acquire)) {
        snapshot = {};
        return false;
    }
    snapshot.buttons = s_touch.buttons;
    snapshot.lx = s_touch.left.x;
    snapshot.ly = s_touch.left.y;
    snapshot.rx = s_touch.right.x;
    snapshot.ry = s_touch.right.y;
    return true;
}

void GCController_SetTouchMotionEnabled(bool enabled)
{
    std::scoped_lock lock(s_touchMutex);
    if (s_touchMotionEnabled.load(std::memory_order_acquire) != enabled) {
        s_touchMotion.reset();
        s_touchMotionTime = {};
    }
    s_touchMotionEnabled.store(enabled, std::memory_order_release);
}

void GCController_UpdateTouchMotion(float dt, float gx, float gy, float gz, float ax, float ay, float az, bool isPortrait)
{
    if (!std::isfinite(dt) || !std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz) ||
        !std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) return;
    std::scoped_lock lock(s_touchMutex);
    if (!s_touchMotionEnabled.load(std::memory_order_acquire)) return;
    const auto generation = s_motionRecenterGeneration.load(std::memory_order_acquire);
    if (generation != s_touchMotionGeneration || isPortrait != s_touchMotionPortrait) {
        s_touchMotion.reset();
        s_touchMotionGeneration = generation;
        s_touchMotionPortrait = isPortrait;
    }
    s_touchMotion.processAppleSample(std::clamp(dt, 0.001f, 0.1f), gx, gy, gz, ax, ay, az);
    s_touchMotionTime = std::chrono::steady_clock::now();
}

AppleGCController::AppleGCController(size_t index, std::string_view uuid, std::string_view display_name)
	: base_type(uuid, display_name), m_index(index)
{
	// GameController motion is opt-out on iPadOS. Existing profiles without a
	// motion setting therefore gain the same automatic setup as buttons/sticks.
	m_settings.motion = true;
	m_settings.axis.deadzone = 0.15f;
	m_settings.rotation.deadzone = 0.15f;
}

bool AppleGCController::connect()
{
	bool connected = is_connected();
	if (connected)
		calibrate();
	return connected;
}

bool AppleGCController::is_connected()
{
	@autoreleasepool {
		return GetController(m_index) != nil || (m_index == 0 &&
            (s_touchEnabled.load(std::memory_order_acquire) || s_touchMotionEnabled.load(std::memory_order_acquire)));
	}
}

bool AppleGCController::has_motion()
{
    if (m_index == 0 && s_touchMotionEnabled.load(std::memory_order_acquire)) {
        std::scoped_lock lock(s_touchMutex);
        if (std::chrono::steady_clock::now() - s_touchMotionTime < std::chrono::milliseconds(250)) return true;
    }
	if (!GCController_IsMotionEnabled())
		return false;
	@autoreleasepool {
		GCMotion* motion = GetController(m_index).motion;
		if (!motion)
			return false;
		return motion.hasRotationRate;
	}
}

MotionSample AppleGCController::get_motion_sample()
{
    if (s_menuCaptured.load(std::memory_order_relaxed)) return {};
    if (m_index == 0 && s_touchMotionEnabled.load(std::memory_order_acquire)) {
        std::scoped_lock lock(s_touchMutex);
        if (std::chrono::steady_clock::now() - s_touchMotionTime < std::chrono::milliseconds(250))
            return s_touchMotion.getMotionSample();
    }
	@autoreleasepool {
		GCMotion* motion = GetController(m_index).motion;
		if (!motion)
			return {};

		if (!motion.hasRotationRate)
			return {};
		if (motion.sensorsRequireManualActivation && !motion.sensorsActive)
			motion.sensorsActive = YES;

		GCAcceleration acceleration{};
		if (motion.hasGravityAndUserAcceleration) {
			const GCAcceleration gravity = motion.gravity;
			const GCAcceleration user = motion.userAcceleration;
			acceleration = {gravity.x + user.x, gravity.y + user.y, gravity.z + user.z};
		} else {
			acceleration = motion.acceleration;
		}

		const GCRotationRate rotation = motion.rotationRate;
		const auto now = std::chrono::steady_clock::now();
		std::scoped_lock lock(m_motionMutex);
		const uint64 recenterGeneration = s_motionRecenterGeneration.load(std::memory_order_acquire);
		if (recenterGeneration != m_motionRecenterGeneration) {
			m_motionHandler.reset();
			m_motionRecenterGeneration = recenterGeneration;
			m_lastMotionSampleTime = {};
		}
		float elapsedSeconds = 1.0f / 60.0f;
		if (m_lastMotionSampleTime.time_since_epoch().count() != 0) {
			elapsedSeconds = std::chrono::duration<float>(now - m_lastMotionSampleTime).count();
			if (elapsedSeconds <= 0.0f || elapsedSeconds > 0.25f)
				elapsedSeconds = 1.0f / 60.0f;
		}
		m_lastMotionSampleTime = now;

        m_motionHandler.processAppleSample(elapsedSeconds,
            static_cast<float>(rotation.x), static_cast<float>(rotation.y), static_cast<float>(rotation.z),
            static_cast<float>(acceleration.x), static_cast<float>(acceleration.y), static_cast<float>(acceleration.z));
		return m_motionHandler.getMotionSample();
	}
}

std::string AppleGCController::get_button_name(uint64 button) const
{
	switch (button) {
		case kButton0: return "A / Cross (Bottom)";
		case kButton1: return "B / Circle (Right)";
		case kButton2: return "X / Square (Left)";
		case kButton3: return "Y / Triangle (Top)";
		case kButton4: return "Options / Minus";
		case kButton5: return "Home";
		case kButton6: return "Menu / Plus";
		case kButton7: return "L3 / Left Thumbstick Button";
		case kButton8: return "R3 / Right Thumbstick Button";
		case kButton9: return "L / Left Shoulder";
		case kButton10: return "R / Right Shoulder";
		case kButton11: return "D-Pad Up";
		case kButton12: return "D-Pad Down";
		case kButton13: return "D-Pad Left";
		case kButton14: return "D-Pad Right";
		case kButtonZL: return "ZL / Left Trigger";
		case kButtonZR: return "ZR / Right Trigger";
		default: break;
	}
	return base_type::get_button_name(button);
}

ControllerState AppleGCController::raw_state()
{
	ControllerState result{};
    if (s_menuCaptured.load(std::memory_order_relaxed)) return result;
	@autoreleasepool {
		GCController* gc = GetController(m_index);

		if ((void*)gc != m_lastAttachedGC) {
			m_lastAttachedGC = (void*)gc;
			if (gc != nil) {
				NSString* name = gc.vendorName ?: @"Game Controller";
				NSString* cat = gc.productCategory ?: @"Unknown";
				NSString* nameLower = name.lowercaseString;
				NSString* catLower = cat.lowercaseString;
				m_usesNintendoLabels = (nameLower && ([nameLower containsString:@"pro controller"] || [nameLower containsString:@"nintendo"] || [nameLower containsString:@"switch"] || [nameLower containsString:@"joy-con"])) ||
				                       (catLower && ([catLower containsString:@"switch"] || [catLower containsString:@"nintendo"]));

				cemuLog_log(LogType::Force, "[GCController] Player {} attached to '{}' (category: {}, nintendoLayout: {})",
					(uint32)m_index + 1, [name UTF8String], [cat UTF8String], m_usesNintendoLabels ? 1 : 0);
				calibrate();
			} else {
				m_usesNintendoLabels = false;
				cemuLog_log(LogType::Force, "[GCController] Player {} controller detached", (uint32)m_index + 1);
			}
		}

		GCExtendedGamepad* ext = gc ? gc.extendedGamepad : nil;
		if (ext) {
			const bool recenterChordPressed = ext.buttonOptions && ext.buttonOptions.isPressed &&
				ext.rightThumbstickButton && ext.rightThumbstickButton.isPressed;
			if (recenterChordPressed && !m_recenterChordWasPressed)
				GCController_RequestMotionRecenter();
			m_recenterChordWasPressed = recenterChordPressed;
			// Face buttons:
			// kButton0 = bottom face button (Xbox A / PS Cross) -> maps to VPAD B in default layout
			// kButton1 = right face button (Xbox B / PS Circle)  -> maps to VPAD A in default layout
			// kButton2 = left face button (Xbox X / PS Square)   -> maps to VPAD Y in default layout
			// kButton3 = top face button (Xbox Y / PS Triangle)  -> maps to VPAD X in default layout
			if (ext.buttonA.isPressed) result.buttons.SetButtonState(m_usesNintendoLabels ? kButton1 : kButton0, true);
			if (ext.buttonB.isPressed) result.buttons.SetButtonState(m_usesNintendoLabels ? kButton0 : kButton1, true);
			if (ext.buttonX.isPressed) result.buttons.SetButtonState(m_usesNintendoLabels ? kButton3 : kButton2, true);
			if (ext.buttonY.isPressed) result.buttons.SetButtonState(m_usesNintendoLabels ? kButton2 : kButton3, true);

			// Shoulder buttons
			if (ext.leftShoulder.isPressed) result.buttons.SetButtonState(kButton9, true);
			if (ext.rightShoulder.isPressed) result.buttons.SetButtonState(kButton10, true);

			// Triggers (support both analog values and digital buttons)
			float lTrig = ext.leftTrigger.value;
			float rTrig = ext.rightTrigger.value;
			if (ext.leftTrigger.isPressed && lTrig < 0.5f) lTrig = 1.0f;
			if (ext.rightTrigger.isPressed && rTrig < 0.5f) rTrig = 1.0f;
			result.trigger.x = lTrig;
			result.trigger.y = rTrig;
			if (ext.leftTrigger.isPressed || lTrig > 0.5f) result.buttons.SetButtonState(kButtonZL, true);
			if (ext.rightTrigger.isPressed || rTrig > 0.5f) result.buttons.SetButtonState(kButtonZR, true);

			// Menu / Options / Home
			if (ext.buttonMenu.isPressed) result.buttons.SetButtonState(kButton6, true);
			if (ext.buttonOptions && ext.buttonOptions.isPressed) result.buttons.SetButtonState(kButton4, true);
			if (ext.buttonHome && ext.buttonHome.isPressed) result.buttons.SetButtonState(kButton5, true);

			// Thumbstick buttons (L3 / R3)
			if (ext.leftThumbstickButton && ext.leftThumbstickButton.isPressed) result.buttons.SetButtonState(kButton7, true);
			if (ext.rightThumbstickButton && ext.rightThumbstickButton.isPressed) result.buttons.SetButtonState(kButton8, true);

			// D-Pad
			if (ext.dpad.up.isPressed) result.buttons.SetButtonState(kButton11, true);
			if (ext.dpad.down.isPressed) result.buttons.SetButtonState(kButton12, true);
			if (ext.dpad.left.isPressed) result.buttons.SetButtonState(kButton13, true);
			if (ext.dpad.right.isPressed) result.buttons.SetButtonState(kButton14, true);

			// Analog sticks
			result.axis.x = ext.leftThumbstick.xAxis.value;
			result.axis.y = -ext.leftThumbstick.yAxis.value;
			result.rotation.x = ext.rightThumbstick.xAxis.value;
			result.rotation.y = -ext.rightThumbstick.yAxis.value;
		// Keep the legacy MFi profile for older physical controllers. Apple has
		// deprecated this API, but removing it would silently drop their input.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		} else if (gc.gamepad) {
			GCGamepad* pad = gc.gamepad;
			if (pad.buttonA.isPressed) result.buttons.SetButtonState(kButton0, true);
			if (pad.buttonB.isPressed) result.buttons.SetButtonState(kButton1, true);
			if (pad.buttonX.isPressed) result.buttons.SetButtonState(kButton2, true);
			if (pad.buttonY.isPressed) result.buttons.SetButtonState(kButton3, true);
			if (pad.leftShoulder.isPressed) result.buttons.SetButtonState(kButton9, true);
			if (pad.rightShoulder.isPressed) result.buttons.SetButtonState(kButton10, true);
			if (pad.dpad.up.isPressed) result.buttons.SetButtonState(kButton11, true);
			if (pad.dpad.down.isPressed) result.buttons.SetButtonState(kButton12, true);
			if (pad.dpad.left.isPressed) result.buttons.SetButtonState(kButton13, true);
			if (pad.dpad.right.isPressed) result.buttons.SetButtonState(kButton14, true);
		}
#pragma clang diagnostic pop
		else if (gc.microGamepad) {
			// Single Joy-Con and compact MFi devices expose the micro profile.
			// It cannot emulate missing sticks/triggers, but navigation and the two
			// face buttons remain usable instead of silently producing no input.
			GCMicroGamepad* pad = gc.microGamepad;
			if (pad.buttonA.isPressed) result.buttons.SetButtonState(kButton0, true);
			if (pad.buttonX.isPressed) result.buttons.SetButtonState(kButton2, true);
			if (pad.buttonMenu.isPressed) result.buttons.SetButtonState(kButton6, true);
			if (pad.dpad.up.isPressed) result.buttons.SetButtonState(kButton11, true);
			if (pad.dpad.down.isPressed) result.buttons.SetButtonState(kButton12, true);
			if (pad.dpad.left.isPressed) result.buttons.SetButtonState(kButton13, true);
			if (pad.dpad.right.isPressed) result.buttons.SetButtonState(kButton14, true);
			result.axis.x = pad.dpad.xAxis.value;
			result.axis.y = -pad.dpad.yAxis.value;
		}
	}
    if (m_index == 0 && s_touchEnabled.load(std::memory_order_acquire)) {
        std::scoped_lock lock(s_touchMutex);
        if (s_touchEnabled.load(std::memory_order_acquire)) {
            for (uint32 button = 0; button <= kButtonZR; ++button)
                if (s_touch.buttons & (1ULL << button)) result.buttons.SetButtonState(button, true);
            if (s_touch.left.x != 0 || s_touch.left.y != 0) result.axis = s_touch.left;
            if (s_touch.right.x != 0 || s_touch.right.y != 0) result.rotation = s_touch.right;
            if (s_touch.buttons & (1ULL << kButtonZL)) result.trigger.x = 1.0f;
            if (s_touch.buttons & (1ULL << kButtonZR)) result.trigger.y = 1.0f;
        }
    }
    return result;
}
