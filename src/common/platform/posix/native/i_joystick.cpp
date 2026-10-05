#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdio>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sys/inotify.h>
#include <sys/ioctl.h>

#include "m_joy.h"
#include "d_eventbase.h"

// Include after the engine key definitions: linux/input.h also defines KEY_ names.
#include <linux/input.h>

EXTERN_CVAR(Bool, use_joystick)

static constexpr int NUM_PAD_AXES = 6;
static constexpr int NUM_ENGINE_BUTTONS = NUM_KEYS;

static const char *AxisNames[NUM_PAD_AXES] = {
	"Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y", "Left Trigger", "Right Trigger"
};

static const EAxisCodes AxisCodes[NUM_PAD_AXES][2] = {
	{ AXIS_CODE_PAD_LTHUMB_RIGHT, AXIS_CODE_PAD_LTHUMB_LEFT },
	{ AXIS_CODE_PAD_LTHUMB_DOWN, AXIS_CODE_PAD_LTHUMB_UP },
	{ AXIS_CODE_PAD_RTHUMB_RIGHT, AXIS_CODE_PAD_RTHUMB_LEFT },
	{ AXIS_CODE_PAD_RTHUMB_DOWN, AXIS_CODE_PAD_RTHUMB_UP },
	{ AXIS_CODE_PAD_LTRIGGER, AXIS_CODE_NULL },
	{ AXIS_CODE_PAD_RTRIGGER, AXIS_CODE_NULL }
};

struct AxisDefaults
{
	float deadZone;
	float threshold;
};

static const AxisDefaults DefaultAxes[NUM_PAD_AXES] = {
	{ JOYDEADZONE_DEFAULT, JOYTHRESH_STICK_X },
	{ JOYDEADZONE_DEFAULT, JOYTHRESH_STICK_Y },
	{ JOYDEADZONE_DEFAULT, JOYTHRESH_STICK_X },
	{ JOYDEADZONE_DEFAULT, JOYTHRESH_STICK_Y },
	{ JOYDEADZONE_DEFAULT, JOYTHRESH_TRIGGER },
	{ JOYDEADZONE_DEFAULT, JOYTHRESH_TRIGGER }
};

static bool TestBit(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / (sizeof(unsigned long) * 8)] & (1UL << (bit % (sizeof(unsigned long) * 8)))) != 0;
}

static int LinuxButtonToEngineKey(unsigned int code)
{
	switch (code)
	{
	case BTN_SOUTH: return KEY_PAD_A;
	case BTN_EAST: return KEY_PAD_B;
	case BTN_WEST: return KEY_PAD_X;
	case BTN_NORTH: return KEY_PAD_Y;
	case BTN_SELECT: return KEY_PAD_BACK;
	case BTN_START: return KEY_PAD_START;
	case BTN_MODE: return KEY_PAD_GUIDE;
	case BTN_THUMBL: return KEY_PAD_LTHUMB;
	case BTN_THUMBR: return KEY_PAD_RTHUMB;
	case BTN_TL: return KEY_PAD_LSHOULDER;
	case BTN_TR: return KEY_PAD_RSHOULDER;
	case BTN_TL2: return KEY_PAD_LTRIGGER;
	case BTN_TR2: return KEY_PAD_RTRIGGER;
	case BTN_DPAD_UP: return KEY_PAD_DPAD_UP;
	case BTN_DPAD_DOWN: return KEY_PAD_DPAD_DOWN;
	case BTN_DPAD_LEFT: return KEY_PAD_DPAD_LEFT;
	case BTN_DPAD_RIGHT: return KEY_PAD_DPAD_RIGHT;
#ifdef BTN_TRIGGER_HAPPY1
	case BTN_TRIGGER_HAPPY1: return KEY_PAD_PADDLE1;
	case BTN_TRIGGER_HAPPY2: return KEY_PAD_PADDLE2;
	case BTN_TRIGGER_HAPPY3: return KEY_PAD_PADDLE3;
	case BTN_TRIGGER_HAPPY4: return KEY_PAD_PADDLE4;
#endif
	case BTN_TOUCH: return KEY_PAD_TOUCHPAD;
	default:
		if ((code >= BTN_0 && code <= BTN_9) || (code >= BTN_JOYSTICK && code <= BTN_DEAD))
			return KEY_FIRSTJOYBUTTON + static_cast<int>(code - BTN_MISC);
		return -1;
	}
}

class EvdevJoystick final : public IJoystickConfig
{
public:
	EvdevJoystick(int fd, bool writable, const char *name, const char *path, const input_id &id,
		const char *phys, const char *uniq)
		: m_fd(fd), m_writable(writable), m_name(name), m_path(path)
	{
		m_axisCodes[0] = ABS_X;
		m_axisCodes[1] = ABS_Y;
		m_axisCodes[2] = ABS_RX;
		m_axisCodes[3] = ABS_RY;
		m_axisCodes[4] = HasAbsAxis(ABS_BRAKE) && HasAbsAxis(ABS_GAS) ? ABS_BRAKE : ABS_Z;
		m_axisCodes[5] = HasAbsAxis(ABS_BRAKE) && HasAbsAxis(ABS_GAS) ? ABS_GAS : ABS_RZ;
		for (int i = 0; i < NUM_PAD_AXES; ++i)
			ReadAxis(i);

		if (uniq && *uniq)
			m_identifier.Format("EV:%04x:%04x:%s", id.vendor, id.product, uniq);
		else if (phys && *phys)
			m_identifier.Format("EV:%04x:%04x:%s", id.vendor, id.product, phys);
		else
			m_identifier.Format("EV:%04x:%04x:%s", id.vendor, id.product, path);

		SetDefaultConfig();
		M_LoadJoystickConfig(this);
		InitRumble();
	}

	~EvdevJoystick() override
	{
		ReleaseInputs();
		StopRumble();
		if (m_effectId >= 0)
			ioctl(m_fd, EVIOCRMFF, m_effectId);
		if (m_settingsChanged)
			M_SaveJoystickConfig(this);
		close(m_fd);
	}

	void Process()
	{
		const bool active = use_joystick && m_enabled;
		if (active != m_wasActive)
		{
			if (active)
				Resynchronize();
			else
			{
				ReleaseInputs();
				StopRumble();
				m_rumbleStrong = m_rumbleWeak = 0;
			}
			m_wasActive = active;
		}

		input_event events[32];
		for (;;)
		{
			const ssize_t bytes = read(m_fd, events, sizeof(events));
			if (bytes > 0)
			{
				const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
				for (size_t i = 0; i < count; ++i)
					ProcessEvent(events[i], active);
				continue;
			}
			if (bytes == 0)
				m_disconnected = true;
			else if (errno == EINTR)
				continue;
			else if (errno != EAGAIN && errno != EWOULDBLOCK)
				m_disconnected = true;
			break;
		}

		if (active)
			ProcessAxes();
	}

	bool IsDisconnected() const { return m_disconnected; }
	const char *GetPath() const { return m_path.GetChars(); }
	void MarkSeen() { m_seen = true; }
	void MarkUnseen() { m_seen = false; }
	bool WasSeen() const { return m_seen; }
	void AddAxes(float axes[NUM_AXIS_CODES]);

	void Rumble(double highFreq, double lowFreq)
	{
		if (!HasHaptics()) return;
		if (!m_enabled || !use_joystick)
		{
			StopRumble();
			m_rumbleStrong = m_rumbleWeak = 0;
			return;
		}
		const uint16_t strong = ToMagnitude(lowFreq * m_hapticsStrength);
		const uint16_t weak = ToMagnitude(highFreq * m_hapticsStrength);
		if (strong == m_rumbleStrong && weak == m_rumbleWeak && m_rumbleActive)
			return;
		if (strong == 0 && weak == 0)
		{
			StopRumble();
			m_rumbleStrong = m_rumbleWeak = 0;
			return;
		}

		StopRumble();
		m_effect.u.rumble.strong_magnitude = strong;
		m_effect.u.rumble.weak_magnitude = weak;
		if (ioctl(m_fd, EVIOCSFF, &m_effect) < 0)
			return;
		input_event event = {};
		event.type = EV_FF;
		event.code = static_cast<unsigned short>(m_effectId);
		event.value = 1;
		if (write(m_fd, &event, sizeof(event)) == sizeof(event))
		{
			m_rumbleActive = true;
			m_rumbleStrong = strong;
			m_rumbleWeak = weak;
		}
	}

	FString GetName() override { return m_name; }
	float GetSensitivity() override { return m_sensitivity; }
	void SetSensitivity(float value) override { m_settingsChanged = true; m_sensitivity = value; }
	bool HasHaptics() override { return m_effectId >= 0; }
	float GetHapticsStrength() override { return HasHaptics() ? m_hapticsStrength : 0.f; }
	void SetHapticsStrength(float value) override
	{
		if (!HasHaptics()) return;
		m_settingsChanged = true;
		m_hapticsStrength = Clamp(value, 0.f, 2.f);
	}
	int GetNumAxes() override { return NUM_PAD_AXES; }
	float GetAxisDeadZone(int axis) override { return ValidAxis(axis) ? m_axes[axis].deadZone : 0.f; }
	const char *GetAxisName(int axis) override { return ValidAxis(axis) ? AxisNames[axis] : "Axis"; }
	float GetAxisScale(int axis) override { return ValidAxis(axis) ? m_axes[axis].scale : 1.f; }
	float GetAxisDigitalThreshold(int axis) override { return ValidAxis(axis) ? m_axes[axis].threshold : 1.f; }
	EJoyCurve GetAxisResponseCurve(int axis) override { return ValidAxis(axis) ? m_axes[axis].curvePreset : JOYCURVE_DEFAULT; }
	float GetAxisResponseCurvePoint(int axis, int point) override
	{
		return ValidAxis(axis) && unsigned(point) < 4 ? m_axes[axis].curve.pts[point] : 0.f;
	}
	void SetAxisDeadZone(int axis, float value) override
	{
		if (ValidAxis(axis)) { m_settingsChanged = true; m_axes[axis].deadZone = Clamp(value, 0.f, 1.f); }
	}
	void SetAxisScale(int axis, float value) override { if (ValidAxis(axis)) { m_settingsChanged = true; m_axes[axis].scale = value; } }
	void SetAxisDigitalThreshold(int axis, float value) override { if (ValidAxis(axis)) { m_settingsChanged = true; m_axes[axis].threshold = value; } }
	void SetAxisResponseCurve(int axis, EJoyCurve preset) override
	{
		if (!ValidAxis(axis) || preset < JOYCURVE_CUSTOM || preset >= NUM_JOYCURVE) return;
		m_settingsChanged = true;
		m_axes[axis].curvePreset = preset;
		if (preset != JOYCURVE_CUSTOM) m_axes[axis].curve = JOYCURVE[preset];
	}
	void SetAxisResponseCurvePoint(int axis, int point, float value) override
	{
		if (!ValidAxis(axis) || unsigned(point) >= 4) return;
		m_settingsChanged = true;
		m_axes[axis].curvePreset = JOYCURVE_CUSTOM;
		m_axes[axis].curve.pts[point] = value;
	}
	bool GetEnabled() override { return m_enabled; }
	void SetEnabled(bool value) override
	{
		m_settingsChanged = true;
		m_enabled = value;
		if (!value)
		{
			StopRumble();
			m_rumbleStrong = m_rumbleWeak = 0;
		}
	}
	bool AllowsEnabledInBackground() override { return true; }
	bool GetEnabledInBackground() override { return true; }
	void SetEnabledInBackground(bool) override {}
	bool IsSensitivityDefault() override { return m_sensitivity == JOYSENSITIVITY_DEFAULT; }
	bool IsHapticsStrengthDefault() override { return m_hapticsStrength == JOYHAPSTRENGTH_DEFAULT; }
	bool IsAxisDeadZoneDefault(int axis) override { return ValidAxis(axis) && m_axes[axis].deadZone == DefaultAxes[axis].deadZone; }
	bool IsAxisScaleDefault(int axis) override { return ValidAxis(axis) && m_axes[axis].scale == JOYSENSITIVITY_DEFAULT; }
	bool IsAxisDigitalThresholdDefault(int axis) override { return ValidAxis(axis) && m_axes[axis].threshold == DefaultAxes[axis].threshold; }
	bool IsAxisResponseCurveDefault(int axis) override { return ValidAxis(axis) && m_axes[axis].curvePreset == JOYCURVE_DEFAULT; }
	void SetDefaultConfig() override
	{
		m_sensitivity = JOYSENSITIVITY_DEFAULT;
		m_hapticsStrength = JOYHAPSTRENGTH_DEFAULT;
		m_enabled = true;
		for (int i = 0; i < NUM_PAD_AXES; ++i)
		{
			m_axes[i].deadZone = DefaultAxes[i].deadZone;
			m_axes[i].scale = JOYSENSITIVITY_DEFAULT;
			m_axes[i].threshold = DefaultAxes[i].threshold;
			m_axes[i].curvePreset = JOYCURVE_DEFAULT;
			m_axes[i].curve = JOYCURVE[JOYCURVE_DEFAULT];
		}
	}
	FString GetIdentifier() override { return m_identifier; }

private:
	struct AxisState
	{
		int code = -1;
		input_absinfo info = {};
		float rawValue = 0.f;
		float value = 0.f;
		float deadZone = JOYDEADZONE_DEFAULT;
		float scale = JOYSENSITIVITY_DEFAULT;
		float threshold = JOYTHRESH_DEFAULT;
		EJoyCurve curvePreset = JOYCURVE_DEFAULT;
		CubicBezier curve = JOYCURVE[JOYCURVE_DEFAULT];
	};

	static bool ValidAxis(int axis) { return unsigned(axis) < NUM_PAD_AXES; }
	static float Clamp(float value, float low, float high)
	{
		if (!std::isfinite(value)) return low;
		return value < low ? low : value > high ? high : value;
	}
	static uint16_t ToMagnitude(double value)
	{
		if (!std::isfinite(value)) value = 0;
		if (value < 0) value = 0;
		if (value > 1) value = 1;
		return static_cast<uint16_t>(value * 65535.0);
	}
	bool HasAbsAxis(int code) const
	{
		input_absinfo info = {};
		return ioctl(m_fd, EVIOCGABS(code), &info) >= 0 && info.maximum > info.minimum;
	}
	void ReadAxis(int axis)
	{
		AxisState &state = m_axes[axis];
		state.code = m_axisCodes[axis];
		state.value = 0.f;
		if (ioctl(m_fd, EVIOCGABS(state.code), &state.info) < 0 || state.info.maximum <= state.info.minimum)
			state.code = -1;
	}
	void SetRawAxis(int axis, int value)
	{
		AxisState &state = m_axes[axis];
		if (state.code < 0) return;
		const double minimum = state.info.minimum;
		const double maximum = state.info.maximum;
		double normalized;
		if (axis == 4 || axis == 5)
			normalized = (value - minimum) / (maximum - minimum);
		else
		{
			const double center = (minimum + maximum) * 0.5;
			normalized = value >= center
				? (value - center) / (maximum - center)
				: (value - center) / (center - minimum);
		}
		state.rawValue = static_cast<float>(normalized < -1 ? -1 : normalized > 1 ? 1 : normalized);
	}
	void ProcessAxes()
	{
		uint8_t state = 0;
		double x = m_axes[0].rawValue, y = m_axes[1].rawValue;
		Joy_ManageThumbstick(&x, &y, m_axes[0].deadZone, m_axes[1].deadZone,
			m_axes[0].threshold, m_axes[1].threshold, m_axes[0].curve, m_axes[1].curve, &state);
		m_axes[0].value = static_cast<float>(x);
		m_axes[1].value = static_cast<float>(y);
		SetAxisButtons(0, state, 4, KEY_PAD_LTHUMB_RIGHT);

		x = m_axes[2].rawValue; y = m_axes[3].rawValue; state = 0;
		Joy_ManageThumbstick(&x, &y, m_axes[2].deadZone, m_axes[3].deadZone,
			m_axes[2].threshold, m_axes[3].threshold, m_axes[2].curve, m_axes[3].curve, &state);
		m_axes[2].value = static_cast<float>(x);
		m_axes[3].value = static_cast<float>(y);
		SetAxisButtons(2, state, 4, KEY_PAD_RTHUMB_RIGHT);

		for (int axis = 4; axis < NUM_PAD_AXES; ++axis)
		{
			state = 0;
			m_axes[axis].value = static_cast<float>(Joy_ManageSingleAxis(m_axes[axis].rawValue,
				m_axes[axis].deadZone, m_axes[axis].threshold, m_axes[axis].curve, &state));
			SetAxisButtons(axis, state, 1, axis == 4 ? KEY_PAD_LTRIGGER : KEY_PAD_RTRIGGER);
		}
	}
	void SetAxisButtons(int axis, uint8_t state, int count, int base)
	{
		m_axisButtons[axis] = state;
		for (int i = 0; i < count; ++i)
			SyncEngineButton(base + i);
	}
	void SyncEngineButton(int key)
	{
		if (key < 0 || key >= NUM_ENGINE_BUTTONS) return;
		bool desired = m_physicalButtons[key];
		if (key >= KEY_PAD_DPAD_UP && key <= KEY_PAD_DPAD_RIGHT)
		{
			if (key == KEY_PAD_DPAD_UP) desired |= m_hatY < 0;
			if (key == KEY_PAD_DPAD_DOWN) desired |= m_hatY > 0;
			if (key == KEY_PAD_DPAD_LEFT) desired |= m_hatX < 0;
			if (key == KEY_PAD_DPAD_RIGHT) desired |= m_hatX > 0;
		}
		if (key >= KEY_PAD_LTHUMB_RIGHT && key <= KEY_PAD_LTHUMB_UP)
			desired |= (m_axisButtons[0] & (1u << (key - KEY_PAD_LTHUMB_RIGHT))) != 0;
		if (key >= KEY_PAD_RTHUMB_RIGHT && key <= KEY_PAD_RTHUMB_UP)
			desired |= (m_axisButtons[2] & (1u << (key - KEY_PAD_RTHUMB_RIGHT))) != 0;
		if (key == KEY_PAD_LTRIGGER) desired |= (m_axisButtons[4] & 1) != 0;
		if (key == KEY_PAD_RTRIGGER) desired |= (m_axisButtons[5] & 1) != 0;
		if (desired != m_engineButtons[key])
		{
			Joy_GenerateButtonEvent(desired, static_cast<EKeyCodes>(key));
			m_engineButtons[key] = desired;
		}
	}
	void SyncAllEngineButtons()
	{
		for (int key = 0; key < NUM_ENGINE_BUTTONS; ++key) SyncEngineButton(key);
	}
	void SetPhysicalButton(int code, bool down, bool active)
	{
		if (code < 0 || code > KEY_MAX) return;
		const int key = LinuxButtonToEngineKey(code);
		if (key >= 0 && key < NUM_ENGINE_BUTTONS && active)
		{
			m_physicalButtons[key] = down;
			SyncEngineButton(key);
		}
	}
	void SetHatAxis(int code, int value, bool active)
	{
		if (code == ABS_HAT0X) m_hatX = value < 0 ? -1 : value > 0 ? 1 : 0;
		if (code == ABS_HAT0Y) m_hatY = value < 0 ? -1 : value > 0 ? 1 : 0;
		if (active)
		{
			SyncEngineButton(KEY_PAD_DPAD_UP);
			SyncEngineButton(KEY_PAD_DPAD_DOWN);
			SyncEngineButton(KEY_PAD_DPAD_LEFT);
			SyncEngineButton(KEY_PAD_DPAD_RIGHT);
		}
	}
	void ProcessEvent(const input_event &event, bool active)
	{
		if (event.type == EV_SYN && event.code == SYN_DROPPED)
		{
			m_synDropped = true;
			return;
		}
		if (m_synDropped)
		{
			if (event.type == EV_SYN && event.code == SYN_REPORT)
			{
				m_synDropped = false;
				if (active) Resynchronize();
			}
			return;
		}
		if (event.type == EV_KEY) SetPhysicalButton(event.code, event.value != 0, active);
		else if (event.type == EV_ABS)
		{
			for (int i = 0; i < NUM_PAD_AXES; ++i)
				if (m_axes[i].code == event.code) SetRawAxis(i, event.value);
			if (event.code == ABS_HAT0X || event.code == ABS_HAT0Y) SetHatAxis(event.code, event.value, active);
		}
	}
	void Resynchronize()
	{
		unsigned long keys[(KEY_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)] = {};
		if (ioctl(m_fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
		{
			memset(m_physicalButtons, 0, sizeof(m_physicalButtons));
			for (int code = 0; code <= KEY_MAX; ++code)
			{
				if (!TestBit(keys, code)) continue;
				const int key = LinuxButtonToEngineKey(code);
				if (key >= 0 && key < NUM_ENGINE_BUTTONS) m_physicalButtons[key] = true;
			}
		}
		else
		{
			// A failed state query must release old physical-button state; keeping
			// it would leave controls stuck after an incomplete evdev resync.
			memset(m_physicalButtons, 0, sizeof(m_physicalButtons));
		}
		input_absinfo info = {};
		if (ioctl(m_fd, EVIOCGABS(ABS_HAT0X), &info) >= 0) m_hatX = info.value < 0 ? -1 : info.value > 0 ? 1 : 0;
		else m_hatX = 0;
		if (ioctl(m_fd, EVIOCGABS(ABS_HAT0Y), &info) >= 0) m_hatY = info.value < 0 ? -1 : info.value > 0 ? 1 : 0;
		else m_hatY = 0;
		for (int i = 0; i < NUM_PAD_AXES; ++i)
			if (m_axes[i].code >= 0)
			{
				if (ioctl(m_fd, EVIOCGABS(m_axes[i].code), &info) >= 0)
				{
					m_axes[i].info = info;
					SetRawAxis(i, info.value);
				}
				else
				{
					// Likewise, a failed axis query cannot safely preserve the last
					// value observed before events were dropped.
					m_axes[i].rawValue = 0.f;
				}
			}
		SyncAllEngineButtons();
	}
	void ReleaseInputs()
	{
		memset(m_physicalButtons, 0, sizeof(m_physicalButtons));
		m_hatX = m_hatY = 0;
		for (int i = 0; i < NUM_PAD_AXES; ++i)
		{
			m_axes[i].rawValue = 0.f;
			m_axes[i].value = 0.f;
		}
		ProcessAxes();
		SyncAllEngineButtons();
	}
	void InitRumble()
	{
		if (!m_writable) return;
		unsigned long ffbits[(FF_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)] = {};
		if (ioctl(m_fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits) < 0 || !TestBit(ffbits, FF_RUMBLE)) return;
		memset(&m_effect, 0, sizeof(m_effect));
		m_effect.type = FF_RUMBLE;
		m_effect.id = -1;
		m_effect.replay.length = 32767;
		if (ioctl(m_fd, EVIOCSFF, &m_effect) >= 0) m_effectId = m_effect.id;
	}
	void StopRumble()
	{
		if (!m_rumbleActive || m_effectId < 0) return;
		input_event event = {};
		event.type = EV_FF;
		event.code = static_cast<unsigned short>(m_effectId);
		event.value = 0;
		write(m_fd, &event, sizeof(event));
		m_rumbleActive = false;
	}

	int m_fd;
	bool m_writable;
	FString m_name;
	FString m_path;
	FString m_identifier;
	float m_sensitivity = JOYSENSITIVITY_DEFAULT;
	float m_hapticsStrength = JOYHAPSTRENGTH_DEFAULT;
	bool m_enabled = true;
	bool m_settingsChanged = false;
	bool m_wasActive = false;
	bool m_seen = false;
	bool m_disconnected = false;
	bool m_synDropped = false;
	int m_axisCodes[NUM_PAD_AXES] = {};
	AxisState m_axes[NUM_PAD_AXES];
	bool m_physicalButtons[NUM_ENGINE_BUTTONS] = {};
	bool m_engineButtons[NUM_ENGINE_BUTTONS] = {};
	uint8_t m_axisButtons[NUM_PAD_AXES] = {};
	int m_hatX = 0;
	int m_hatY = 0;
	ff_effect m_effect = {};
	int m_effectId = -1;
	bool m_rumbleActive = false;
	uint16_t m_rumbleStrong = 0;
	uint16_t m_rumbleWeak = 0;
};

void EvdevJoystick::AddAxes(float axes[NUM_AXIS_CODES])
{
	if (!m_enabled) return;
	for (int i = 0; i < NUM_PAD_AXES; ++i)
	{
		const float value = m_axes[i].value * m_sensitivity * m_axes[i].scale;
		const int code = value > 0 ? AxisCodes[i][0] : value < 0 ? AxisCodes[i][1] : AXIS_CODE_NULL;
		if (code != AXIS_CODE_NULL) axes[code] += std::fabs(value);
	}
}

static TArray<EvdevJoystick *> Joysticks;
static int inotifyFd = -1;
static int inotifyWatch = -1;
static bool initialScanPending = true;

static bool IsGamepad(int fd)
{
	unsigned long eventBits[(EV_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)] = {};
	unsigned long keyBits[(KEY_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)] = {};
	unsigned long absBits[(ABS_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)] = {};
	if (ioctl(fd, EVIOCGBIT(0, sizeof(eventBits)), eventBits) < 0) return false;
	const bool hasKeys = TestBit(eventBits, EV_KEY) && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0;
	const bool hasAxes = TestBit(eventBits, EV_ABS) && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0;
	const bool gamepadKey = hasKeys && (TestBit(keyBits, BTN_GAMEPAD) || TestBit(keyBits, BTN_JOYSTICK));
	bool gamepadPaddles = false;
#ifdef BTN_TRIGGER_HAPPY1
	gamepadPaddles = hasKeys && hasAxes && TestBit(keyBits, BTN_TRIGGER_HAPPY1)
		&& TestBit(absBits, ABS_X) && TestBit(absBits, ABS_Y);
#endif
	return gamepadKey || gamepadPaddles;
}

static void TryAddDevice(const char *filename)
{
	if (strncmp(filename, "event", 5) != 0) return;
	char path[256];
	snprintf(path, sizeof(path), "/dev/input/%s", filename);
	for (unsigned int i = 0; i < Joysticks.Size(); ++i)
	{
		if (strcmp(Joysticks[i]->GetPath(), path) == 0)
		{
			Joysticks[i]->MarkSeen();
			return;
		}
	}
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	bool writable = fd >= 0;
	if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return;
	if (!IsGamepad(fd)) { close(fd); return; }

	char name[256] = {};
	char phys[256] = {};
	char uniq[256] = {};
	input_id id = {};
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 || !name[0]) strcpy(name, "Unknown Gamepad");
	ioctl(fd, EVIOCGID, &id);
	ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
	ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq);
	Joysticks.Push(new EvdevJoystick(fd, writable, name, path, id, phys, uniq));
	printf("EvdevJoystick: Added %s (%s)\n", name, path);
}

static void RescanDevices()
{
	DIR *dir = opendir("/dev/input");
	if (!dir) return;
	for (unsigned int i = 0; i < Joysticks.Size(); ++i) Joysticks[i]->MarkUnseen();
	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) TryAddDevice(entry->d_name);
	closedir(dir);
	for (unsigned int i = 0; i < Joysticks.Size();)
	{
		if (!Joysticks[i]->WasSeen() || Joysticks[i]->IsDisconnected())
		{
			printf("EvdevJoystick: Removed %s (%s)\n", Joysticks[i]->GetName().GetChars(), Joysticks[i]->GetPath());
			delete Joysticks[i];
			Joysticks.Delete(i);
		}
		else ++i;
	}
}

void I_StartupJoysticks()
{
	inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotifyFd >= 0)
		inotifyWatch = inotify_add_watch(inotifyFd, "/dev/input", IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_ATTRIB | IN_Q_OVERFLOW);
	initialScanPending = true;
}

void I_ShutdownInput()
{
	if (inotifyWatch >= 0 && inotifyFd >= 0) inotify_rm_watch(inotifyFd, inotifyWatch);
	if (inotifyFd >= 0) close(inotifyFd);
	inotifyWatch = inotifyFd = -1;
	for (unsigned int i = 0; i < Joysticks.Size(); ++i) delete Joysticks[i];
	Joysticks.Clear();
}

void I_PollJoystickDeviceChanges()
{
	bool changed = initialScanPending;
	initialScanPending = false;
	for (unsigned int i = 0; i < Joysticks.Size(); ++i)
		if (Joysticks[i]->IsDisconnected()) changed = true;
	if (inotifyFd >= 0)
	{
		alignas(inotify_event) char buffer[4096];
		for (;;)
		{
			const ssize_t bytes = read(inotifyFd, buffer, sizeof(buffer));
			if (bytes < 0 && errno == EINTR) continue;
			if (bytes <= 0) break;
			for (size_t offset = 0; offset + sizeof(inotify_event) <= static_cast<size_t>(bytes);)
			{
				const auto *event = reinterpret_cast<const inotify_event *>(buffer + offset);
				if (event->mask & (IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_ATTRIB | IN_Q_OVERFLOW)) changed = true;
				offset += sizeof(inotify_event) + event->len;
			}
		}
	}
	if (changed)
	{
		event_t event = {};
		event.type = EV_DeviceChange;
		D_PostEvent(&event);
	}
}

void I_GetJoysticks(TArray<IJoystickConfig *> &sticks)
{
	sticks.Clear();
	for (unsigned int i = 0; i < Joysticks.Size(); ++i) sticks.Push(Joysticks[i]);
}

void I_ProcessJoysticks()
{
	for (unsigned int i = 0; i < Joysticks.Size(); ++i) Joysticks[i]->Process();
}

IJoystickConfig *I_UpdateDeviceList()
{
	RescanDevices();
	return nullptr;
}

void I_GetAxes(float axes[NUM_AXIS_CODES])
{
	for (int i = 0; i < NUM_AXIS_CODES; ++i) axes[i] = 0.f;
	if (!use_joystick) return;
	for (unsigned int i = 0; i < Joysticks.Size(); ++i) Joysticks[i]->AddAxes(axes);
}

void I_Rumble(double high_freq, double low_freq, double, double)
{
	if (!use_joystick) high_freq = low_freq = 0;
	for (unsigned int i = 0; i < Joysticks.Size(); ++i) Joysticks[i]->Rumble(high_freq, low_freq);
}

void I_JoyConsumeEvent(int instanceID, event_t *event) {}
