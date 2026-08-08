#include "st_start.h"
#include "v_video.h"

class NativeStartupScreen : public FStartupScreen {
public:
    NativeStartupScreen(int max_progress) : FStartupScreen(max_progress) {}
    void Progress(int advance = 1) override {}
    void LoadingStatus(const char *message, int colors) override {}
    void NetInit(const char *message, bool host) override {}
};

FStartupScreen *FStartupScreen::CreateInstance(int max_progress)
{
    return new NativeStartupScreen(max_progress);
}
