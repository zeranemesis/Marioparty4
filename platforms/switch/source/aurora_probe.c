// Aurora on libnx, exercised the way the game uses it: GX geometry through
// Aurora's TEV shaders, Dawn's OpenGL ES backend at the Compatibility feature
// level, presented on the NWindow. Each frame clears the EFB to a cycling
// colour, then draws
//   - a spinning triangle with per-vertex colours (vertex format, colour
//     channel, PASSCLR TEV stage, position matrix),
//   - a textured, tinted quad (an I8 checkerboard in GameCube tile layout,
//     texture coordinate generation, MODULATE TEV stage),
// under an orthographic projection of the 640x480 EFB. A black screen, a
// frozen frame or missing shapes point at the failing stage.
//
// The same source builds on a desktop host to check Aurora at Dawn's
// Compatibility level on OpenGL ES (Mesa) without a console:
//   --frames N         quit after N frames;
//   --backend vulkan   compare with another Dawn backend (default: OpenGL ES).
// + (START) quits.

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EFB_WIDTH 640
#define EFB_HEIGHT 480
#define TEX_SIZE 64

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  (void)len;
  static const char* const kLevels[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
  const char* name = (unsigned)level < sizeof(kLevels) / sizeof(kLevels[0]) ? kLevels[level] : "?";
  printf("[%s: %s] %s\n", name, module, message);
  fflush(stdout);
  if (level == LOG_FATAL) {
    abort();
  }
}

static u8 wave(unsigned frame, unsigned offset) {
  const unsigned t = (frame + offset) % 240;
  return (u8)(t < 120 ? t : 240 - t);
}

// GX_TF_I8 stores 8x4 texel tiles. With 8x8 checker squares every tile is a
// single value, so the checkerboard is just one byte per tile.
static u8 s_checker[TEX_SIZE * TEX_SIZE] __attribute__((aligned(32)));
static u8 s_fifo[0x100000] __attribute__((aligned(32)));

static void make_checker(void) {
  const int tilesPerRow = TEX_SIZE / 8;
  const int tileRows = TEX_SIZE / 4;
  for (int ty = 0; ty < tileRows; ++ty) {
    for (int tx = 0; tx < tilesPerRow; ++tx) {
      const int x = tx * 8;
      const int y = ty * 4;
      const u8 value = (((x / 8) + (y / 8)) & 1) ? 0xFF : 0x40;
      memset(s_checker + (size_t)(ty * tilesPerRow + tx) * 32, value, 32);
    }
  }
}

static void setup_common(void) {
  Mtx44 projection;
  C_MTXOrtho(projection, 0.0f, (f32)EFB_HEIGHT, 0.0f, (f32)EFB_WIDTH, 0.0f, 1.0f);
  GXSetProjection(projection, GX_ORTHOGRAPHIC);
  GXSetViewport(0.0f, 0.0f, (f32)EFB_WIDTH, (f32)EFB_HEIGHT, 0.0f, 1.0f);
  GXSetScissor(0, 0, EFB_WIDTH, EFB_HEIGHT);
  GXSetCullMode(GX_CULL_NONE);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTevStages(1);
}

static void draw_triangle(unsigned frame) {
  Mtx rotation, translation, modelView;
  C_MTXRotRad(rotation, 'z', (f32)frame * 0.03f);
  C_MTXTrans(translation, 200.0f, 240.0f, -0.5f);
  C_MTXConcat(translation, rotation, modelView);
  GXLoadPosMtxImm(modelView, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);

  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetNumTexGens(0);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

  GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
  GXPosition3f32(0.0f, -120.0f, 0.0f);
  GXColor4u8(255, 64, 64, 255);
  GXPosition3f32(104.0f, 60.0f, 0.0f);
  GXColor4u8(64, 255, 64, 255);
  GXPosition3f32(-104.0f, 60.0f, 0.0f);
  GXColor4u8(64, 64, 255, 255);
  GXEnd();
}

static void draw_textured_quad(unsigned frame, GXTexObj* texture) {
  Mtx modelView;
  C_MTXTrans(modelView, 440.0f, 240.0f + 40.0f * sinf((f32)frame * 0.05f), -0.5f);
  GXLoadPosMtxImm(modelView, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);

  GXLoadTexObj(texture, GX_TEXMAP0);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);

  const f32 h = 96.0f;
  GXBegin(GX_QUADS, GX_VTXFMT1, 4);
  GXPosition3f32(-h, -h, 0.0f);
  GXColor4u8(255, 255, 255, 255);
  GXTexCoord2f32(0.0f, 0.0f);
  GXPosition3f32(h, -h, 0.0f);
  GXColor4u8(255, 220, 120, 255);
  GXTexCoord2f32(2.0f, 0.0f);
  GXPosition3f32(h, h, 0.0f);
  GXColor4u8(120, 220, 255, 255);
  GXTexCoord2f32(2.0f, 2.0f);
  GXPosition3f32(-h, h, 0.0f);
  GXColor4u8(255, 255, 255, 255);
  GXTexCoord2f32(0.0f, 2.0f);
  GXEnd();
}

int main(int argc, char* argv[]) {
  long maxFrames = -1;
  AuroraBackend backend = BACKEND_OPENGLES;
  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], "--frames") == 0) {
      maxFrames = strtol(argv[i + 1], NULL, 10);
    } else if (strcmp(argv[i], "--backend") == 0 && strcmp(argv[i + 1], "vulkan") == 0) {
      backend = BACKEND_VULKAN;
    }
  }

  const AuroraConfig config = {
      .appName = "PartyBoard Aurora probe",
      .desiredBackend = backend,
      .allowCpuAdapter = true, // host checks run on Mesa's software rasterisers
      .logCallback = &log_callback,
  };
  aurora_initialize(argc, argv, &config);
  // As the game does at startup (src/game/init.c): GXInit sets the whole GX
  // state (TEV swap tables, colours, matrices) to the hardware defaults.
  GXInit(s_fifo, sizeof(s_fifo));
  PADInit();

  make_checker();
  GXTexObj checker;
  GXInitTexObj(&checker, s_checker, TEX_SIZE, TEX_SIZE, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
  GXInitTexObjLOD(&checker, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);

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
    setup_common();
    draw_triangle(frame);
    draw_textured_quad(frame, &checker);
    aurora_end_frame();
    ++frame;
    if (maxFrames >= 0 && (long)frame >= maxFrames) {
      exiting = true;
    }
  }

  aurora_shutdown();
  return 0;
}
