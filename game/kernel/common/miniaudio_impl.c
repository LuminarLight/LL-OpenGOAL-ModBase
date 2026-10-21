// stb_vorbis must be declared before miniaudio for .ogg support
#define STB_VORBIS_HEADER_ONLY
#include "third-party/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
// AppleClang does not define POSIX macros, which makes miniaudio redefine stdlib functions
#if defined(__APPLE__)
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#include "third-party/miniaudio.h"
#undef _POSIX_C_SOURCE
#else
#define NOT_REAL_OLD_POSIX_C_SOURCE _POSIX_C_SOURCE
#include "third-party/miniaudio.h"
#define _POSIX_C_SOURCE NOT_REAL_OLD_POSIX_C_SOURCE
#undef NOT_REAL_OLD_POSIX_C_SOURCE
#endif
#else
#include "third-party/miniaudio.h"
#endif

#undef STB_VORBIS_HEADER_ONLY
#include "third-party/stb_vorbis.c"
