// platform/ipados/Bridge/WindowSystemIOS.mm
//
// Real headless iPadOS platform implementation of the Cemu WindowSystem interface.
// Provides window state, physical pixel dimensions, and the surface pointer to CAMetalLayer
// with zero desktop UI or wxWidgets dependencies.

#include "Common/precompiled.h"
#include "gui/interface/WindowSystem.h"
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <os/log.h>

namespace WindowSystem
{
    static WindowInfo s_windowInfo{};

    void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory> /*errorCategory*/)
    {
        os_log_error(OS_LOG_DEFAULT, "[WindowSystem] ERROR DIALOG: %{public}.*s - %{public}.*s",
                     (int)title.size(), title.data(),
                     (int)message.size(), message.data());
    }

    void Create()
    {
    }

    WindowInfo& GetWindowInfo()
    {
        return s_windowInfo;
    }

    void UpdateWindowTitles(bool /*isIdle*/, bool /*isLoading*/, double /*fps*/)
    {
    }

    void GetWindowSize(int& w, int& h)
    {
        w = s_windowInfo.width.load();
        h = s_windowInfo.height.load();
    }

    void GetPadWindowSize(int& w, int& h)
    {
        if (s_windowInfo.pad_open.load())
        {
            w = s_windowInfo.pad_width.load();
            h = s_windowInfo.pad_height.load();
        }
        else
        {
            w = 0;
            h = 0;
        }
    }

    void GetWindowPhysSize(int& w, int& h)
    {
        w = s_windowInfo.phys_width.load();
        h = s_windowInfo.phys_height.load();
    }

    void GetPadWindowPhysSize(int& w, int& h)
    {
        if (s_windowInfo.pad_open.load())
        {
            w = s_windowInfo.phys_pad_width.load();
            h = s_windowInfo.phys_pad_height.load();
        }
        else
        {
            w = 0;
            h = 0;
        }
    }

    double GetWindowDPIScale()
    {
        return s_windowInfo.dpi_scale.load();
    }

    double GetPadDPIScale()
    {
        return s_windowInfo.pad_open.load() ? s_windowInfo.pad_dpi_scale.load() : 1.0;
    }

    bool IsPadWindowOpen()
    {
        return s_windowInfo.pad_open.load();
    }

    bool IsKeyDown(uint32 /*key*/)
    {
        return false;
    }

    bool IsKeyDown(PlatformKeyCodes /*key*/)
    {
        return false;
    }

    std::string GetKeyCodeName(uint32 /*key*/)
    {
        return "";
    }

    bool InputConfigWindowHasFocus()
    {
        return false;
    }

    void NotifyGameLoaded()
    {
    }

    void NotifyGameExited()
    {
    }

    void RefreshGameList()
    {
    }

    bool IsFullScreen()
    {
        return true;
    }

    bool IsPhone()
    {
        return [UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPhone;
    }

    void CaptureInput(const ControllerState& /*currentState*/, const ControllerState& /*lastState*/)
    {
    }
} // namespace WindowSystem
