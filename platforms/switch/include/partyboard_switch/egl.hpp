#pragma once

#include <cstdint>

#include <EGL/egl.h>
#include <switch.h>

// Thin libnx/EGL ownership layer shared by the bootstrap today and the
// Aurora/Dawn Switch overlay later. Keeping EGL lifetime here prevents the
// compatibility layer from depending directly on libnx window setup details.

bool PartyBoardSwitch_EglInitialize();
void PartyBoardSwitch_EglShutdown();

EGLDisplay PartyBoardSwitch_EglDisplay();
EGLContext PartyBoardSwitch_EglContext();
EGLSurface PartyBoardSwitch_EglSurface();

NWindow* PartyBoardSwitch_NativeWindow();

uint32_t PartyBoardSwitch_FramebufferWidth();
uint32_t PartyBoardSwitch_FramebufferHeight();

bool PartyBoardSwitch_SwapBuffers();

// Dawn's OpenGL backend refuses adapter discovery without robust-context
// creation plus an EGL sync primitive. This checks those exact requirements.
bool PartyBoardSwitch_EglHasDawnRequirements();
