// dr_wav, dr_mp3 and dr_flac are compiled once here (the audio tile's
// decoders; stb_vorbis.c compiles on its own).
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO // files are read through SDL's IO streams
#include "dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
