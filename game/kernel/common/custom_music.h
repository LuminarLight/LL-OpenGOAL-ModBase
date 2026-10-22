#pragma once

#include "common/common_types.h"

void custom_music_init_engine();
void custom_music_shutdown();
void custom_music_update(u32 name, u32 volume_bits, u32 paused, u32 variant);
