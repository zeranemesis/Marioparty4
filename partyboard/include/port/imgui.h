#ifndef PORT_IMGUI_H_
#define PORT_IMGUI_H_

#include <aurora/aurora.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

void imgui_main(const AuroraInfo* info);
void frame_limiter();
int frame_pacer_simulation_tick();
void frame_pacer_commit_simulation_tick();
void frame_pacer_reset();
float frame_pacer_interpolation_step();
bool frame_pacer_interpolation_enabled();
const char* imgui_get_image_path_from_popup();

#ifdef __cplusplus
}
#endif

#endif
