#include "common.h"
#include "ui.h"

int main(int argc, char *argv[])
{
    ui_init();

    while (aptMainLoop()) {
        if (!ui_frame()) break;
    }

    ui_exit();
    return 0;
}
