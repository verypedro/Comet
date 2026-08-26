#pragma once

typedef enum {
    ICON_COMET,
    ICON_BADGE_3D,
    ICON_BADGE_2D,
    ICON_BTN_A,
    ICON_BTN_B,
    ICON_BTN_X,
    ICON_BTN_Y,
    ICON_BTN_R,
    ICON_BTN_L,
    ICON_BTN_SELECT,
    ICON_BTN_START,
    ICON_BADGE_DS_TAR,
    ICON_BADGE_DS_EXTRACTED,
    ICON_TAB_L,
    ICON_TAB_R,
    ICON_COUNT,
} IconId;

typedef struct {
    const unsigned char *data;
    unsigned short texW, texH;
    unsigned short imgW, imgH;
} EmbeddedIcon;

extern const EmbeddedIcon g_embeddedIcons[ICON_COUNT];
