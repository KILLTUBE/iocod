include_guard(GLOBAL)

set(RENDERER_COMMON_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_font.c
    ${SOURCE_DIR}/renderercommon/tr_image_bmp.c
    ${SOURCE_DIR}/renderercommon/tr_image_jpg.c
    ${SOURCE_DIR}/renderercommon/tr_image_pcx.c
    ${SOURCE_DIR}/renderercommon/tr_image_png.c
    ${SOURCE_DIR}/renderercommon/tr_image_pvr.c
    ${SOURCE_DIR}/renderercommon/tr_image_tga.c
    ${SOURCE_DIR}/renderercommon/tr_noise.c
    ${SOURCE_DIR}/renderercommon/puff.c
)

set(SDL_RENDERER_SOURCES
    ${SOURCE_DIR}/sdl/sdl_gamma.c
    ${SOURCE_DIR}/sdl/sdl_glimp.c
)

set(DYNAMIC_RENDERER_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_subs.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/q_math.c
)

if(USE_FREETYPE)
    list(APPEND RENDERER_DEFINITIONS BUILD_FREETYPE)
endif()

if(BUILD_STANDALONE)
    list(APPEND RENDERER_DEFINITIONS STANDALONE)
endif()

if(USE_RENDERER_DLOPEN)
    list(APPEND RENDERER_DEFINITIONS USE_RENDERER_DLOPEN)
elseif(NOT BUILD_RENDERER_GL2)
    message(FATAL_ERROR "No static renderer enabled; choose GL2")
endif()

list(APPEND RENDERER_LIBRARIES ${COMMON_LIBRARIES})
