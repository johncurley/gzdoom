#ifndef __NATIVE_I_INPUT_H__
#define __NATIVE_I_INPUT_H__

// Native input interface definitions
void I_GetEvent();
void I_StartTic();
void I_StartFrame();
void I_SetMouseCapture();
void I_ReleaseMouseCapture();
void I_SetNativeMouse(bool wantNative);
void I_SetWindowFocus(bool focused);

#endif
