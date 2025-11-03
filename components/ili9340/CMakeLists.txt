set(srcs "ili9340.c" "fontx.c")

idf_component_register(SRCS "ili9340.c" "fontx.c"
                       REQUIRES driver
                       INCLUDE_DIRS ".")
