// Aurora on libnx: Aurora's own "simple" example (initialise, then clear the
// screen through GX every frame), with the clear colour cycling so a frozen
// frame is obvious. Reaching the screen means SDL3 window -> Dawn OpenGL ES
// surface on the NWindow -> Aurora's GX/WebGPU frame loop all work.
//
// + (START) quits.

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>

#include <stdio.h>
#include <stdlib.h>

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  (void)len;
  static const char* const kLevels[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
  const char* name = (unsigned)level < sizeof(kLevels) / sizeof(kLevels[0]) ? kLevels[level] : "?";
  printf("[%s: %s] %s\n", name, module, message);
  if (level == LOG_FATAL) {
    fflush(stdout);
    abort();
  }
}

static u8 wave(unsigned frame, unsigned offset) {
  const unsigned t = (frame + offset) % 240;
  return (u8)(t < 120 ? t * 2 : (240 - t) * 2);
}

int main(int argc, char* argv[]) {
  const AuroraConfig config = {
      .appName = "PartyBoard Aurora probe",
      .logCallback = &log_callback,
  };
  aurora_initialize(argc, argv, &config);
  PADInit();

  unsigned frame = 0;
  bool exiting = false;
  bool paused = false;
  while (!exiting) {
    const AuroraEvent* event = aurora_update();
    while (event != NULL && event->type != AURORA_NONE) {
      switch (event->type) {
      case AURORA_EXIT:
        exiting = true;
        break;
      case AURORA_PAUSED:
        paused = true;
        break;
      case AURORA_UNPAUSED:
        paused = false;
        break;
      default:
        break;
      }
      ++event;
    }

    PADStatus pads[PAD_MAX_CONTROLLERS];
    PADRead(pads);
    for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
      if (pads[i].err == PAD_ERR_NONE && (pads[i].button & PAD_BUTTON_START)) {
        exiting = true;
      }
    }

    if (exiting || paused || !aurora_begin_frame()) {
      continue;
    }
    GXSetCopyClear((GXColor){wave(frame, 0), wave(frame, 80), wave(frame, 160), 255}, GX_MAX_Z24);
    aurora_end_frame();
    ++frame;
  }

  aurora_shutdown();
  return 0;
}
