#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdio>
#include <vector>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <cstring>

#include "m_joy.h"
#include "tarray.h"
#include "d_eventbase.h"

// Manual definitions for evdev to avoid linux/input.h conflicts
#define EV_KEY 0x01
#define EV_ABS 0x03
#define BTN_GAMEPAD 0x130
#define KEY_CNT 0x300
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)

struct native_input_event {
	struct timeval time;
	unsigned short type;
	unsigned short code;
	unsigned int value;
};

class EvdevJoystick : public IJoystickConfig
{
    int fd;
    FString name;
    FString devicePath;
    float sensitivity = 1.0f;
    bool enabled = true;

public:
    EvdevJoystick(int fd, const char* name, const char* path) : fd(fd), name(name), devicePath(path) {}
    ~EvdevJoystick() { close(fd); }

    FString GetPath() const { return devicePath; }

    FString GetName() override { return name; }
    float GetSensitivity() override { return sensitivity; }
    void SetSensitivity(float scale) override { sensitivity = scale; }

    bool HasHaptics() override { return false; }
    float GetHapticsStrength() override { return 0; }
    void SetHapticsStrength(float strength) override {}

    int GetNumAxes() override { return 6; } // Standard gamepad axes
    float GetAxisDeadZone(int axis) override { return 0.1f; }
    const char *GetAxisName(int axis) override { return "Axis"; }
    float GetAxisScale(int axis) override { return 1.0f; }
    float GetAxisDigitalThreshold(int axis) override { return 0.5f; }
    EJoyCurve GetAxisResponseCurve(int axis) override { return JOYCURVE_LINEAR; }
    float GetAxisResponseCurvePoint(int axis, int point) override { return 0; }

    void SetAxisDeadZone(int axis, float zone) override {}
    void SetAxisScale(int axis, float scale) override {}
    void SetAxisDigitalThreshold(int axis, float threshold) override {}
    void SetAxisResponseCurve(int axis, EJoyCurve preset) override {}
    void SetAxisResponseCurvePoint(int axis, int point, float value) override {}

    bool GetEnabled() override { return enabled; }
    void SetEnabled(bool e) override { enabled = e; }

    bool AllowsEnabledInBackground() override { return true; }
    bool GetEnabledInBackground() override { return true; }
    void SetEnabledInBackground(bool enabled) override {}

    bool IsSensitivityDefault() override { return true; }
    bool IsHapticsStrengthDefault() override { return true; }
    bool IsAxisDeadZoneDefault(int axis) override { return true; }
    bool IsAxisScaleDefault(int axis) override { return true; }
    bool IsAxisDigitalThresholdDefault(int axis) override { return true; }
    bool IsAxisResponseCurveDefault(int axis) override { return true; }

    void SetDefaultConfig() override {}
    FString GetIdentifier() override { return name; }

    void Process() {
        struct native_input_event ev;
        while (read(fd, &ev, sizeof(ev)) > 0) {
            // Processing logic...
        }
    }
};

static TArray<IJoystickConfig*> Joysticks;
static int inotifyFd = -1;

static void TryAddDevice(const char* filename) {
    if (strncmp(filename, "event", 5) != 0) return;

    char path[256];
    snprintf(path, sizeof(path), "/dev/input/%s", filename);

    // Check if already added
    for (unsigned int i = 0; i < Joysticks.Size(); ++i) {
        if (static_cast<EvdevJoystick*>(Joysticks[i])->GetPath().Compare(path) == 0) return;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        unsigned long keybit[KEY_CNT / (sizeof(unsigned long) * 8) + 1];
        memset(keybit, 0, sizeof(keybit));
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
        
        // Simple check for gamepad buttons
        if (keybit[BTN_GAMEPAD / (sizeof(unsigned long) * 8)] & (1UL << (BTN_GAMEPAD % (sizeof(unsigned long) * 8)))) {
            char name[256];
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) strncpy(name, "Unknown Gamepad", sizeof(name));
            Joysticks.Push(new EvdevJoystick(fd, name, path));
            printf("EvdevJoystick: Added %s (%s)\n", name, path);
        } else {
            close(fd);
        }
    }
}

void I_StartupJoysticks() {
    DIR* dir = opendir("/dev/input");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        TryAddDevice(ent->d_name);
    }
    closedir(dir);

    inotifyFd = inotify_init1(IN_NONBLOCK);
    if (inotifyFd >= 0) {
        inotify_add_watch(inotifyFd, "/dev/input", IN_CREATE | IN_DELETE);
    }
}

void I_ShutdownInput() {
    if (inotifyFd >= 0) close(inotifyFd);
    for (unsigned int i = 0; i < Joysticks.Size(); ++i) {
        delete Joysticks[i];
    }
    Joysticks.Clear();
}

void I_GetJoysticks(TArray<IJoystickConfig *> &sticks) {
    sticks = Joysticks;
}

void I_ProcessJoysticks() {
    for (unsigned int i = 0; i < Joysticks.Size(); ++i) {
        static_cast<EvdevJoystick*>(Joysticks[i])->Process();
    }
}

IJoystickConfig *I_UpdateDeviceList() {
    if (inotifyFd < 0) return nullptr;

    char buffer[4096];
    ssize_t length = read(inotifyFd, buffer, sizeof(buffer));
    if (length < 0) return nullptr;

    int i = 0;
    while (i < (int)length) {
        struct inotify_event* event = (struct inotify_event*)&buffer[i];
        if (event->len) {
            if (event->mask & IN_CREATE) {
                TryAddDevice(event->name);
            }
        }
        i += sizeof(struct inotify_event) + event->len;
    }
    return nullptr;
}

void I_GetAxes(float axes[NUM_AXIS_CODES]) {
    for (int i = 0; i < NUM_AXIS_CODES; i++) axes[i] = 0;
}

void I_Rumble(double, double, double, double) {}

void I_JoyConsumeEvent(int instanceID, event_t * event) {}
