set(FONTS jetbrains_mono_42 jetbrains_mono_76 jetbrains_mono_bold_20
   jetbrains_mono_extrabold_compressed lv_font_sys_48
   open_sans_light fontawesome_weathericons)
find_program(LV_FONT_CONV "lv_font_conv" NO_CACHE REQUIRED
   HINTS "${CMAKE_SOURCE_DIR}/node_modules/.bin")
message(STATUS "Using ${LV_FONT_CONV} to generate font files")
configure_file(${CMAKE_CURRENT_LIST_DIR}/jetbrains_mono_bold_20.c_zero.patch
   ${CMAKE_CURRENT_BINARY_DIR}/jetbrains_mono_bold_20.c_zero.patch COPYONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/jetbrains_mono_bold_20.c_M.patch
   ${CMAKE_CURRENT_BINARY_DIR}/jetbrains_mono_bold_20.c_M.patch COPYONLY)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.12)
   # FindPython3 module introduces with CMake 3.12
   # https://cmake.org/cmake/help/latest/module/FindPython3.html
   set(Python3_FIND_STRATEGY LOCATION) # https://discourse.cmake.org/t/find-package-python3-is-not-finding-the-correct-python/10563
   find_package(Python3 REQUIRED)
else()
   set(Python3_EXECUTABLE "python")
endif()

# create static library building fonts
add_library(infinitime_fonts STATIC)
# add include directory to lvgl headers needed to compile the font files on its own
target_include_directories(infinitime_fonts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libs")
foreach(FONT ${FONTS})
   add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${FONT}.c
      COMMAND "${Python3_EXECUTABLE}" ${CMAKE_CURRENT_SOURCE_DIR}/generate.py
      --lv-font-conv "${LV_FONT_CONV}"
      --font ${FONT} ${CMAKE_CURRENT_SOURCE_DIR}/fonts.json
      # This fork const-qualifies LV_FONT_DECLARE / LV_FONT_CUSTOM_DECLARE, and
      # its lv_font_montserrat_*.c were patched to match. lv_font_conv only
      # emits a const definition for LVGL >= 8, so on this LVGL 7 tree every
      # generated font is non-const and the build dies with
      #   error: conflicting type qualifiers for '<font>'
      # That went unnoticed for a long time because the stale .obj files were
      # never recompiled; it only surfaces on a clean build. const-qualify the
      # generated definition to match the declaration.
      COMMAND sed -i "s@^lv_font_t ${FONT} = {@const lv_font_t ${FONT} = {@" ${CMAKE_CURRENT_BINARY_DIR}/${FONT}.c
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fonts.json
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
   )
   add_custom_target(infinitime_fonts_${FONT}
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${FONT}.c
   )
   target_sources(infinitime_fonts PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${FONT}.c")
   add_dependencies(infinitime_fonts infinitime_fonts_${FONT})
endforeach()
