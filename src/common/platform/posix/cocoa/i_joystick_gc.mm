/*
** i_joystick_gc.mm
** Modern GameController framework implementation for macOS joystick support
**
**---------------------------------------------------------------------------
**
** Copyright 2025 GZDoom Maintainers and Contributors
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
**---------------------------------------------------------------------------
**
*/

#import <GameController/GameController.h>
#import <Foundation/Foundation.h>

// CoreHaptics is required for haptic feedback support (macOS 11.0+)
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#import <CoreHaptics/CoreHaptics.h>
#endif

#include "d_eventbase.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_joy.h"
#include "v_text.h"
#include "printf.h"
#include "keydef.h"

EXTERN_CVAR(Bool, joy_axespolling)
EXTERN_CVAR(Bool, use_joystick)

// =============================================================================
// Modern GameController-based Joystick Implementation
// =============================================================================

namespace
{

// Axis configuration data
struct AxisInfo
{
	FString name;
	float value = 0.0f;
	float deadZone = JOYDEADZONE_DEFAULT;
	float sensitivity = 1.0f;
	float digitalThreshold = JOYTHRESH_DEFAULT;
	EJoyCurve responseCurvePreset = JOYCURVE_DEFAULT;
	CubicBezier responseCurve = JOYCURVE[JOYCURVE_DEFAULT];

	AxisInfo() = default;
	AxisInfo(const char* axisName) : name(axisName) {}
};

// Button state tracking
struct ButtonState
{
	int oldButtons = 0;
	int newButtons = 0;
};

// =============================================================================
// GameControllerJoystick - Modern implementation using GameController framework
// =============================================================================

class GameControllerJoystick : public IJoystickConfig
{
public:
	explicit GameControllerJoystick(GCController* controller);
	virtual ~GameControllerJoystick();

	// IJoystickConfig interface
	virtual FString GetName() override;
	virtual float GetSensitivity() override;
	virtual void SetSensitivity(float scale) override;

	virtual bool HasHaptics() override;
	virtual float GetHapticsStrength() override;
	virtual void SetHapticsStrength(float strength) override;

	virtual int GetNumAxes() override;
	virtual float GetAxisDeadZone(int axis) override;
	virtual const char* GetAxisName(int axis) override;
	virtual float GetAxisScale(int axis) override;
	virtual float GetAxisDigitalThreshold(int axis) override;
	virtual EJoyCurve GetAxisResponseCurve(int axis) override;
	virtual float GetAxisResponseCurvePoint(int axis, int point) override;

	virtual void SetAxisDeadZone(int axis, float zone) override;
	virtual void SetAxisScale(int axis, float scale) override;
	virtual void SetAxisDigitalThreshold(int axis, float threshold) override;
	virtual void SetAxisResponseCurve(int axis, EJoyCurve preset) override;
	virtual void SetAxisResponseCurvePoint(int axis, int point, float value) override;

	virtual bool GetEnabled() override;
	virtual void SetEnabled(bool enabled) override;

	virtual bool AllowsEnabledInBackground() override;
	virtual bool GetEnabledInBackground() override;
	virtual void SetEnabledInBackground(bool enabled) override;

	virtual bool IsSensitivityDefault() override;
	virtual bool IsHapticsStrengthDefault() override;
	virtual bool IsAxisDeadZoneDefault(int axis) override;
	virtual bool IsAxisScaleDefault(int axis) override;
	virtual bool IsAxisDigitalThresholdDefault(int axis) override;
	virtual bool IsAxisResponseCurveDefault(int axis) override;

	virtual void SetDefaultConfig() override;
	virtual FString GetIdentifier() override;

	// Update methods
	void Update(double currentTime);
	void ProcessInput();
	void AddAxesToArray(float axes[NUM_AXIS_CODES]);
	void SendRumble(double highFreq, double lowFreq, double leftTrig, double rightTrig);

	bool IsConnected() const;

	// Allow GameControllerManager to access m_controller for comparison
	GCController* m_controller; // Strong reference (retained)

private:
	void SetupInputHandlers();
	void UpdateAxisValues();
	void ProcessButtons();

	bool IsAxisValid(int axis) const {
		return axis >= 0 && static_cast<size_t>(axis) < m_axes.Size();
	}

	TArray<AxisInfo> m_axes;
	ButtonState m_buttonState;

	FString m_name;
	FString m_identifier;
	float m_sensitivity = JOYSENSITIVITY_DEFAULT;
	float m_hapticsStrength = JOYHAPSTRENGTH_DEFAULT;
	bool m_enabled = true;
	bool m_enabledInBackground = false;
	bool m_hasHaptics = false;

	id m_pauseHandler = nil;

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
	// Haptics engine for macOS 11.0+ builds
	CHHapticEngine* m_hapticsEngine API_AVAILABLE(macos(11.0)) = nil;
#endif
};

// =============================================================================
// GameControllerJoystick Implementation
// =============================================================================

GameControllerJoystick::GameControllerJoystick(GCController* controller)
	: m_controller(controller) // ARC handles memory management automatically
{
	if (!m_controller)
		return;

	// Get controller name
	if (m_controller.vendorName)
		m_name = [m_controller.vendorName UTF8String];
	else
		m_name = "Game Controller";

	// Create unique identifier
	if (@available(macOS 10.15, *)) {
		if (m_controller.productCategory)
			m_identifier.Format("%s_%p", [m_controller.productCategory UTF8String], (void*)m_controller);
		else
			m_identifier.Format("controller_%p", (void*)m_controller);
	} else {
		m_identifier.Format("controller_%p", (void*)m_controller);
	}

	// Check for haptics support (macOS 11.0+)
	if (@available(macOS 11.0, *)) {
		m_hasHaptics = (m_controller.haptics != nil);
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
		if (m_hasHaptics) {
			// Try to get the haptics engine using runtime selector check
			// GCDeviceHaptics.engine property may not be available in all SDK versions
			id haptics = m_controller.haptics;
			if ([haptics respondsToSelector:@selector(engine)]) {
				m_hapticsEngine = [haptics performSelector:@selector(engine)];
				if (!m_hapticsEngine) {
					m_hasHaptics = false;
				}
			} else {
				// SDK doesn't support engine property
				m_hasHaptics = false;
			}
		}
#endif
	}

	// Setup axes based on controller type
	if (m_controller.extendedGamepad) {
		// Full gamepad with dual sticks
		m_axes.Push(AxisInfo("Left Stick X"));
		m_axes.Push(AxisInfo("Left Stick Y"));
		m_axes.Push(AxisInfo("Right Stick X"));
		m_axes.Push(AxisInfo("Right Stick Y"));
		m_axes.Push(AxisInfo("Left Trigger"));
		m_axes.Push(AxisInfo("Right Trigger"));
	} else if (m_controller.microGamepad) {
		// Simple controller (e.g., Apple TV remote)
		m_axes.Push(AxisInfo("D-Pad X"));
		m_axes.Push(AxisInfo("D-Pad Y"));
	}

	SetupInputHandlers();

	Printf("GameController connected: %s (haptics: %s)\n",
		m_name.GetChars(),
		m_hasHaptics ? "yes" : "no");
}

GameControllerJoystick::~GameControllerJoystick()
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
	// Stop and release haptics engine
	if (@available(macOS 11.0, *)) {
		if (m_hapticsEngine) {
			[m_hapticsEngine stopWithCompletionHandler:nil];
			m_hapticsEngine = nil;
		}
	}
#endif

	// ARC automatically releases m_controller and m_pauseHandler
	m_controller = nil;
	m_pauseHandler = nil;
}

void GameControllerJoystick::SetupInputHandlers()
{
	if (!m_controller)
		return;

	GCExtendedGamepad* gamepad = m_controller.extendedGamepad;
	if (!gamepad)
		return;

	// Handle pause button
	__block GameControllerJoystick* weakSelf = this;
	m_controller.controllerPausedHandler = ^(GCController* controller) {
		if (weakSelf && weakSelf->m_enabled) {
			// Generate pause/menu button event
			event_t event;
			event.type = EV_KeyDown;
			event.data1 = KEY_ESCAPE;
			D_PostEvent(&event);
		}
	};
}

void GameControllerJoystick::UpdateAxisValues()
{
	if (!m_controller || !m_enabled)
		return;

	GCExtendedGamepad* gamepad = m_controller.extendedGamepad;
	if (gamepad && m_axes.Size() >= 6) {
		// Update stick values
		m_axes[0].value = gamepad.leftThumbstick.xAxis.value;
		m_axes[1].value = gamepad.leftThumbstick.yAxis.value;
		m_axes[2].value = gamepad.rightThumbstick.xAxis.value;
		m_axes[3].value = gamepad.rightThumbstick.yAxis.value;
		m_axes[4].value = gamepad.leftTrigger.value;
		m_axes[5].value = gamepad.rightTrigger.value;
	} else if (GCMicroGamepad* micro = m_controller.microGamepad) {
		if (m_axes.Size() >= 2) {
			m_axes[0].value = micro.dpad.xAxis.value;
			m_axes[1].value = micro.dpad.yAxis.value;
		}
	}
}

void GameControllerJoystick::ProcessButtons()
{
	if (!m_controller || !m_enabled)
		return;

	GCExtendedGamepad* gamepad = m_controller.extendedGamepad;
	if (!gamepad)
		return;

	m_buttonState.oldButtons = m_buttonState.newButtons;
	m_buttonState.newButtons = 0;

	// Map buttons to bit flags
	if (gamepad.buttonA.isPressed) m_buttonState.newButtons |= (1 << 0);
	if (gamepad.buttonB.isPressed) m_buttonState.newButtons |= (1 << 1);
	if (gamepad.buttonX.isPressed) m_buttonState.newButtons |= (1 << 2);
	if (gamepad.buttonY.isPressed) m_buttonState.newButtons |= (1 << 3);
	if (gamepad.leftShoulder.isPressed) m_buttonState.newButtons |= (1 << 4);
	if (gamepad.rightShoulder.isPressed) m_buttonState.newButtons |= (1 << 5);
	if (gamepad.leftTrigger.isPressed) m_buttonState.newButtons |= (1 << 6);
	if (gamepad.rightTrigger.isPressed) m_buttonState.newButtons |= (1 << 7);

	// D-pad
	if (gamepad.dpad.up.isPressed) m_buttonState.newButtons |= (1 << 8);
	if (gamepad.dpad.down.isPressed) m_buttonState.newButtons |= (1 << 9);
	if (gamepad.dpad.left.isPressed) m_buttonState.newButtons |= (1 << 10);
	if (gamepad.dpad.right.isPressed) m_buttonState.newButtons |= (1 << 11);

	// Additional buttons (if available)
	if (@available(macOS 10.15, *)) {
		if (gamepad.buttonOptions && gamepad.buttonOptions.isPressed)
			m_buttonState.newButtons |= (1 << 12);
		if (gamepad.buttonMenu && gamepad.buttonMenu.isPressed)
			m_buttonState.newButtons |= (1 << 13);
	}

	// Generate button events for changes
	static const int buttonKeys[] = {
		KEY_PAD_A, KEY_PAD_B, KEY_PAD_X, KEY_PAD_Y,
		KEY_PAD_LSHOULDER, KEY_PAD_RSHOULDER,
		KEY_PAD_LTRIGGER, KEY_PAD_RTRIGGER,
		KEY_PAD_DPAD_UP, KEY_PAD_DPAD_DOWN, KEY_PAD_DPAD_LEFT, KEY_PAD_DPAD_RIGHT,
		KEY_PAD_BACK, KEY_PAD_START
	};

	Joy_GenerateButtonEvents(m_buttonState.oldButtons, m_buttonState.newButtons, 14, buttonKeys);
}

void GameControllerJoystick::Update(double currentTime)
{
	UpdateAxisValues();
	ProcessButtons();
}

void GameControllerJoystick::ProcessInput()
{
	// ProcessInput is called to update button states
	// Axis values are retrieved separately via GetAxes/AddAxesToArray
}

void GameControllerJoystick::AddAxesToArray(float axes[NUM_AXIS_CODES])
{
	if (!m_enabled)
		return;

	// Add this joystick's axes to the provided array
	if (m_axes.Size() >= 6) {
		axes[0] += m_axes[0].value * m_axes[0].sensitivity * m_sensitivity;  // Left X
		axes[1] += m_axes[1].value * m_axes[1].sensitivity * m_sensitivity;  // Left Y
		axes[2] += m_axes[2].value * m_axes[2].sensitivity * m_sensitivity;  // Right X
		axes[3] += m_axes[3].value * m_axes[3].sensitivity * m_sensitivity;  // Right Y
		axes[4] += m_axes[4].value * m_axes[4].sensitivity * m_sensitivity;  // Left Trigger
		axes[5] += m_axes[5].value * m_axes[5].sensitivity * m_sensitivity;  // Right Trigger
	}
}

void GameControllerJoystick::SendRumble(double highFreq, double lowFreq, double leftTrig, double rightTrig)
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
	// Full haptics support for macOS 11.0+ builds
	if (!m_hasHaptics || m_hapticsStrength <= 0.0f)
		return;

	if (@available(macOS 11.0, *)) {
		if (!m_hapticsEngine)
			return;

		NSError* error = nil;

		// Check if engine supports the running property, start if needed
		BOOL isRunning = NO;
		if ([m_hapticsEngine respondsToSelector:@selector(isRunning)]) {
			isRunning = [(id)m_hapticsEngine isRunning];
		}

		if (!isRunning) {
			[m_hapticsEngine startAndReturnError:&error];
			if (error) {
				Printf("Failed to start haptics engine: %s\n", [[error localizedDescription] UTF8String]);
				return;
			}
		}

		// Create haptic pattern combining high and low frequency rumble
		float intensity = fmin(1.0f, (highFreq + lowFreq) * m_hapticsStrength);
		float sharpness = highFreq > lowFreq ? 0.8f : 0.2f;  // High freq = sharp, low freq = dull

		if (intensity > 0.01f) {
			CHHapticEventParameter* intensityParam =
				[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
			CHHapticEventParameter* sharpnessParam =
				[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness];

			NSArray<CHHapticEventParameter*>* params = @[intensityParam, sharpnessParam];

			CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
																  parameters:params
																relativeTime:0
																	duration:0.1]; // 100ms pulse

			CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event]
																	parameters:@[]
																		 error:&error];

			if (!error) {
				id<CHHapticPatternPlayer> player = [m_hapticsEngine createPlayerWithPattern:pattern error:&error];
				if (!error && player) {
					[player startAtTime:0 error:&error];
				}
			}

			if (error) {
				Printf("Haptics playback error: %s\n", [[error localizedDescription] UTF8String]);
			}
		}
	}
#else
	// Stub for older deployment targets (macOS < 11.0)
	(void)highFreq;
	(void)lowFreq;
	(void)leftTrig;
	(void)rightTrig;
#endif
}

bool GameControllerJoystick::IsConnected() const
{
	return m_controller != nil && m_controller.isAttachedToDevice;
}

// IJoystickConfig implementation

FString GameControllerJoystick::GetName()
{
	return m_name;
}

float GameControllerJoystick::GetSensitivity()
{
	return m_sensitivity;
}

void GameControllerJoystick::SetSensitivity(float scale)
{
	m_sensitivity = scale;
}

bool GameControllerJoystick::HasHaptics()
{
	return m_hasHaptics;
}

float GameControllerJoystick::GetHapticsStrength()
{
	return m_hapticsStrength;
}

void GameControllerJoystick::SetHapticsStrength(float strength)
{
	// Clamp strength between 0.0 and 1.0
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 1.0f) strength = 1.0f;
	m_hapticsStrength = strength;
}

int GameControllerJoystick::GetNumAxes()
{
	return static_cast<int>(m_axes.Size());
}

float GameControllerJoystick::GetAxisDeadZone(int axis)
{
	return IsAxisValid(axis) ? m_axes[axis].deadZone : 0.0f;
}

const char* GameControllerJoystick::GetAxisName(int axis)
{
	return IsAxisValid(axis) ? m_axes[axis].name.GetChars() : "Invalid";
}

float GameControllerJoystick::GetAxisScale(int axis)
{
	return IsAxisValid(axis) ? m_axes[axis].sensitivity : 1.0f;
}

float GameControllerJoystick::GetAxisDigitalThreshold(int axis)
{
	return IsAxisValid(axis) ? m_axes[axis].digitalThreshold : JOYTHRESH_DEFAULT;
}

EJoyCurve GameControllerJoystick::GetAxisResponseCurve(int axis)
{
	return IsAxisValid(axis) ? m_axes[axis].responseCurvePreset : JOYCURVE_DEFAULT;
}

float GameControllerJoystick::GetAxisResponseCurvePoint(int axis, int point)
{
	if (!IsAxisValid(axis) || point < 0 || point >= 4)
		return 0.0f;
	return m_axes[axis].responseCurve.pts[point];
}

void GameControllerJoystick::SetAxisDeadZone(int axis, float zone)
{
	if (IsAxisValid(axis))
		m_axes[axis].deadZone = zone;
}

void GameControllerJoystick::SetAxisScale(int axis, float scale)
{
	if (IsAxisValid(axis))
		m_axes[axis].sensitivity = scale;
}

void GameControllerJoystick::SetAxisDigitalThreshold(int axis, float threshold)
{
	if (IsAxisValid(axis))
		m_axes[axis].digitalThreshold = threshold;
}

void GameControllerJoystick::SetAxisResponseCurve(int axis, EJoyCurve preset)
{
	if (IsAxisValid(axis)) {
		m_axes[axis].responseCurvePreset = preset;
		if (preset >= 0 && preset < NUM_JOYCURVE)
			m_axes[axis].responseCurve = JOYCURVE[preset];
	}
}

void GameControllerJoystick::SetAxisResponseCurvePoint(int axis, int point, float value)
{
	if (IsAxisValid(axis) && point >= 0 && point < 4) {
		m_axes[axis].responseCurve.pts[point] = value;
		m_axes[axis].responseCurvePreset = JOYCURVE_CUSTOM;
	}
}

bool GameControllerJoystick::GetEnabled()
{
	return m_enabled;
}

void GameControllerJoystick::SetEnabled(bool enabled)
{
	m_enabled = enabled;
}

bool GameControllerJoystick::AllowsEnabledInBackground()
{
	return true;  // GameController framework supports background operation
}

bool GameControllerJoystick::GetEnabledInBackground()
{
	return m_enabledInBackground;
}

void GameControllerJoystick::SetEnabledInBackground(bool enabled)
{
	m_enabledInBackground = enabled;
}

bool GameControllerJoystick::IsSensitivityDefault()
{
	return m_sensitivity == JOYSENSITIVITY_DEFAULT;
}

bool GameControllerJoystick::IsHapticsStrengthDefault()
{
	return m_hapticsStrength == JOYHAPSTRENGTH_DEFAULT;
}

bool GameControllerJoystick::IsAxisDeadZoneDefault(int axis)
{
	return IsAxisValid(axis) && m_axes[axis].deadZone == JOYDEADZONE_DEFAULT;
}

bool GameControllerJoystick::IsAxisScaleDefault(int axis)
{
	return IsAxisValid(axis) && m_axes[axis].sensitivity == 1.0f;
}

bool GameControllerJoystick::IsAxisDigitalThresholdDefault(int axis)
{
	return IsAxisValid(axis) && m_axes[axis].digitalThreshold == JOYTHRESH_DEFAULT;
}

bool GameControllerJoystick::IsAxisResponseCurveDefault(int axis)
{
	return IsAxisValid(axis) && m_axes[axis].responseCurvePreset == JOYCURVE_DEFAULT;
}

void GameControllerJoystick::SetDefaultConfig()
{
	m_sensitivity = JOYSENSITIVITY_DEFAULT;
	m_hapticsStrength = JOYHAPSTRENGTH_DEFAULT;

	for (unsigned int i = 0; i < m_axes.Size(); ++i) {
		m_axes[i].deadZone = JOYDEADZONE_DEFAULT;
		m_axes[i].sensitivity = 1.0f;
		m_axes[i].digitalThreshold = JOYTHRESH_DEFAULT;
		m_axes[i].responseCurvePreset = JOYCURVE_DEFAULT;
		m_axes[i].responseCurve = JOYCURVE[JOYCURVE_DEFAULT];
	}
}

FString GameControllerJoystick::GetIdentifier()
{
	return m_identifier;
}

// =============================================================================
// GameControllerManager - Manages all connected controllers
// =============================================================================

class GameControllerManager
{
public:
	GameControllerManager();
	~GameControllerManager();

	void Rescan();
	void ProcessInput();
	void AddAxes(float axes[NUM_AXIS_CODES]);
	void GetJoysticks(TArray<IJoystickConfig*>& sticks);
	void Rumble(double highFreq, double lowFreq, double leftTrig, double rightTrig);

private:
	void OnControllerConnected(GCController* controller);
	void OnControllerDisconnected(GCController* controller);

	TArray<GameControllerJoystick*> m_joysticks;
	id m_connectObserver = nil;
	id m_disconnectObserver = nil;
};

GameControllerManager::GameControllerManager()
{
	// Register for controller connection notifications
	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];

	__block GameControllerManager* weakSelf = this;

	m_connectObserver = [center addObserverForName:GCControllerDidConnectNotification
											 object:nil
											  queue:nil
										 usingBlock:^(NSNotification* note) {
		GCController* controller = note.object;
		if (controller && weakSelf) {
			weakSelf->OnControllerConnected(controller);
		}
	}]; // ARC retains automatically

	m_disconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification
												object:nil
												 queue:nil
											usingBlock:^(NSNotification* note) {
		GCController* controller = note.object;
		if (controller && weakSelf) {
			weakSelf->OnControllerDisconnected(controller);
		}
	}]; // ARC retains automatically

	// Scan for already connected controllers
	Rescan();
}

GameControllerManager::~GameControllerManager()
{
	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];

	if (m_connectObserver) {
		[center removeObserver:m_connectObserver];
		m_connectObserver = nil; // ARC releases automatically
	}

	if (m_disconnectObserver) {
		[center removeObserver:m_disconnectObserver];
		m_disconnectObserver = nil; // ARC releases automatically
	}

	for (auto joy : m_joysticks) {
		delete joy;
	}
	m_joysticks.Clear();
}

void GameControllerManager::OnControllerConnected(GCController* controller)
{
	// Check if already tracked
	for (auto joy : m_joysticks) {
		if (joy->IsConnected() && [controller isEqual:joy->m_controller]) {
			return;
		}
	}

	// Add new controller
	GameControllerJoystick* newJoy = new GameControllerJoystick(controller);
	m_joysticks.Push(newJoy);

	// Load saved configuration
	M_LoadJoystickConfig(newJoy);

	// Notify UI
	UpdateJoystickMenu(newJoy);
}

void GameControllerManager::OnControllerDisconnected(GCController* controller)
{
	for (unsigned int i = 0; i < m_joysticks.Size(); ++i) {
		if ([controller isEqual:m_joysticks[i]->m_controller]) {
			delete m_joysticks[i];
			m_joysticks.Delete(i);
			UpdateJoystickMenu(nullptr);
			break;
		}
	}
}

void GameControllerManager::Rescan()
{
	NSArray<GCController*>* controllers = [GCController controllers];

	for (GCController* controller in controllers) {
		OnControllerConnected(controller);
	}
}

void GameControllerManager::ProcessInput()
{
	if (!use_joystick)
		return;

	double currentTime = CFAbsoluteTimeGetCurrent();

	for (auto joy : m_joysticks) {
		if (joy->IsConnected() && joy->GetEnabled()) {
			joy->Update(currentTime);
			joy->ProcessInput();
		}
	}
}

void GameControllerManager::AddAxes(float axes[NUM_AXIS_CODES])
{
	if (!use_joystick)
		return;

	for (auto joy : m_joysticks) {
		if (joy->IsConnected() && joy->GetEnabled()) {
			joy->AddAxesToArray(axes);
		}
	}
}

void GameControllerManager::GetJoysticks(TArray<IJoystickConfig*>& sticks)
{
	for (auto joy : m_joysticks) {
		if (joy->IsConnected()) {
			sticks.Push(joy);
		}
	}
}

void GameControllerManager::Rumble(double highFreq, double lowFreq, double leftTrig, double rightTrig)
{
	for (auto joy : m_joysticks) {
		if (joy->IsConnected() && joy->GetEnabled() && joy->HasHaptics()) {
			joy->SendRumble(highFreq, lowFreq, leftTrig, rightTrig);
		}
	}
}

// Global manager instance
static GameControllerManager* s_manager = nullptr;

} // anonymous namespace

// =============================================================================
// Public API Implementation
// =============================================================================

void I_GetAxes(float axes[NUM_AXIS_CODES])
{
	// Initialize all axes to zero
	for (size_t i = 0; i < NUM_AXIS_CODES; ++i)
	{
		axes[i] = 0.0f;
	}

	// Add axes from all connected joysticks
	if (use_joystick && s_manager)
	{
		s_manager->AddAxes(axes);
	}
}

void I_GetJoysticks(TArray<IJoystickConfig*>& sticks)
{
	if (s_manager) {
		s_manager->GetJoysticks(sticks);
	}
}

IJoystickConfig* I_UpdateDeviceList()
{
	if (!s_manager) {
		s_manager = new GameControllerManager();
	} else {
		s_manager->Rescan();
	}

	TArray<IJoystickConfig*> sticks;
	s_manager->GetJoysticks(sticks);

	return sticks.Size() > 0 ? sticks[0] : nullptr;
}

void I_ProcessJoysticks()
{
	if (s_manager) {
		s_manager->ProcessInput();
	}
}

void I_Rumble(double high_freq, double low_freq, double left_trig, double right_trig)
{
	if (!use_joystick || !s_manager)
		return;

	s_manager->Rumble(high_freq, low_freq, left_trig, right_trig);
}

void I_ShutdownJoysticks()
{
	if (s_manager) {
		delete s_manager;
		s_manager = nullptr;
	}
}

void I_ShutdownInput()
{
	I_ShutdownJoysticks();
}
