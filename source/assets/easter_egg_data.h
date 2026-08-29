#pragma once

typedef enum {
    EASTER_EGG_DOGLEFT,
    EASTER_EGG_DOGRIGHT,
    EASTER_EGG_AVATAR,
    EASTER_EGG_IMG_COUNT,
} EasterEggImageId;

typedef struct {
    const unsigned char *data;
    unsigned short texW, texH;
    unsigned short imgW, imgH;
} EasterEggImage;

extern const EasterEggImage g_easterEggImages[EASTER_EGG_IMG_COUNT];
