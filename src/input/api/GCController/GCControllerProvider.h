#pragma once

#include "input/api/ControllerProvider.h"
#include "input/api/InputAPI.h"

struct GCControllerSettings : ControllerProviderSettings
{
	bool operator==(const ControllerProviderSettings&) const override
	{
		return true;
	}
	bool operator==(const GCControllerSettings&) const = default;
};

class GCControllerProvider : public ControllerProvider<GCControllerSettings>
{
public:
	constexpr static auto kAPIType = InputAPI::GCController;

	GCControllerProvider();
	~GCControllerProvider() override;

	InputAPI::Type api() const override { return InputAPI::GCController; }
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

private:
	void* m_connectObserver = nullptr;
	void* m_disconnectObserver = nullptr;
	void* m_becomeCurrentObserver = nullptr;
};
