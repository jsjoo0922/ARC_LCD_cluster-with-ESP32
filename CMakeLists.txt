SET(SOURCES ui_comp_arcspeed.c
    ui_comp_image1.c
    ui_comp_image2.c
    ui_comp.c
    ui_booting.c
    ui_normalday.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c)

add_library(ui ${SOURCES})
