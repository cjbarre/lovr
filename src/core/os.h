#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#pragma once

typedef struct {
  uint32_t width;
  uint32_t height;
  bool fullscreen;
  bool resizable;
  bool debug;
  int vsync;
  int msaa;
  const char* title;
  struct {
    void* data;
    uint32_t width;
    uint32_t height;
  } icon;
} WindowFlags;

typedef enum {
  MOUSE_LEFT,
  MOUSE_RIGHT
} MouseButton;

typedef enum {
  MOUSE_MODE_NORMAL,
  MOUSE_MODE_GRABBED
} MouseMode;

typedef enum {
  KEY_W,
  KEY_A,
  KEY_S,
  KEY_D,
  KEY_Q,
  KEY_E,
  KEY_UP,
  KEY_DOWN,
  KEY_LEFT,
  KEY_RIGHT,
  KEY_ESCAPE,
  KEY_F5
} KeyCode;

typedef enum {
  BUTTON_PRESSED,
  BUTTON_RELEASED
} ButtonAction;

typedef void (*quitCallback)(void);
typedef void (*windowFocusCallback)(bool focused);
typedef void (*windowResizeCallback)(int width, int height);
typedef void (*mouseButtonCallback)(MouseButton button, ButtonAction action);
typedef void (*keyboardCallback)(ButtonAction action, KeyCode key, uint32_t scancode, bool repeat);
typedef void (*textCallback)(uint32_t codepoint);

bool lovrPlatformInit(void);
void lovrPlatformDestroy(void);
const char* lovrPlatformGetName(void);
double lovrPlatformGetTime(void);
void lovrPlatformSetTime(double t);
void lovrPlatformSleep(double seconds);
void lovrPlatformOpenConsole(void);
void lovrPlatformPollEvents(void);
size_t lovrPlatformGetHomeDirectory(char* buffer, size_t size);
size_t lovrPlatformGetDataDirectory(char* buffer, size_t size);
size_t lovrPlatformGetWorkingDirectory(char* buffer, size_t size);
size_t lovrPlatformGetExecutablePath(char* buffer, size_t size);
size_t lovrPlatformGetBundlePath(char* buffer, size_t size, const char** root);
bool lovrPlatformCreateWindow(WindowFlags* flags);
bool lovrPlatformHasWindow(void);
void lovrPlatformGetWindowSize(int* width, int* height);
void lovrPlatformGetFramebufferSize(int* width, int* height);
void lovrPlatformSetSwapInterval(int interval);
void lovrPlatformSwapBuffers(void);
void* lovrPlatformGetProcAddress(const char* function);
void lovrPlatformOnQuitRequest(quitCallback callback);
void lovrPlatformOnWindowFocus(windowFocusCallback callback);
void lovrPlatformOnWindowResize(windowResizeCallback callback);
void lovrPlatformOnMouseButton(mouseButtonCallback callback);
void lovrPlatformOnKeyboardEvent(keyboardCallback callback);
void lovrPlatformOnTextEvent(textCallback callback);
void lovrPlatformGetMousePosition(double* x, double* y);
void lovrPlatformSetMouseMode(MouseMode mode);
bool lovrPlatformIsMouseDown(MouseButton button);
bool lovrPlatformIsKeyDown(KeyCode key);
#ifdef _WIN32
#include <windows.h>
HANDLE lovrPlatformGetWindow(void);
HGLRC lovrPlatformGetContext(void);
#endif
