// This file exists purely to give stb_image_write.h's implementation a
// single home. Do not add STB_IMAGE_WRITE_IMPLEMENTATION anywhere else,
// or you'll get duplicate-symbol linker errors.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_MALLOC(sz)        malloc(sz)
#define STBIW_REALLOC(p, sz)    realloc(p, sz)
#define STBIW_FREE(p)           free(p)
#include "stb_image_write.h"
