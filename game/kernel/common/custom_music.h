#pragma once

#include "common/common_types.h"

void custom_music_init_engine();
void custom_music_shutdown();
u32 play_custom_music(u32 file_name);
u32 stop_custom_music();
u32 custom_music_update(u32 volume_bits, u32 paused, u32 variant);
