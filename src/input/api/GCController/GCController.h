#pragma once

#include "input/api/GCController/GCControllerProvider.h"
#include "input/api/Controller.h"
#include "input/api/GCController/AppleMotionHandler.h"

void GCController_RequestMotionRecenter();
void GCController_SetMotionEnabled(bool enabled);
bool GCController_IsMotionEnabled();
void GCController_RefreshCache();
void GCController_SetMenuCaptured(bool captured);

// UI writes, input worker reads. Touch input is merged only into player one's adapter.
void GCController_SetTouchEnabled(bool enabled);
void GCController_UpdateTouch(uint64 buttons, float lx, float ly, float rx, float ry);
struct GCControllerTouchSnapshot
{
	uint64 buttons = 0;
	float lx = 0.0f;
	float ly = 0.0f;
	float rx = 0.0f;
	float ry = 0.0f;
};
// The on-screen controller has fixed Wii U labels. Reading its state directly
// lets VPAD merge it independently of old or customized hardware mappings.
bool GCController_GetTouchSnapshot(GCControllerTouchSnapshot& snapshot);
void GCController_SetTouchMotionEnabled(bool enabled);
void GCController_UpdateTouchMotion(float dt, float gx, float gy, float gz, float ax, float ay, float az, bool isPortrait = false);

class AppleGCController : public Controller<GCControllerProvider>
{
public:
	AppleGCController(size_t index, std::string_view uuid, std::string_view display_name);
	~AppleGCController() override = default;

	std::string_view api_name() const override
	{
		return to_string(InputAPI::GCController);
	}
	InputAPI::Type api() const override { return InputAPI::GCController; }

	bool connect() override;
	bool is_connected() override;
	bool has_motion() override;
	MotionSample get_motion_sample() override;
	size_t get_index() const { return m_index; }

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

private:
	size_t m_index = 0;
	void* m_lastAttachedGC = nullptr;
	bool m_usesNintendoLabels = false;
	std::mutex m_motionMutex;
	std::chrono::steady_clock::time_point m_lastMotionSampleTime{};
	AppleMotionHandler m_motionHandler;
	uint64 m_motionRecenterGeneration = 0;
	bool m_recenterChordWasPressed = false;
};
