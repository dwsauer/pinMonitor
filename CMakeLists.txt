# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS components)
project(pinMonitor)
# Disable Werror specifically for IDF's wpa_supplicant target
idf_component_get_property(WPA_SUPPLICANT_LIB wpa_supplicant COMPONENT_LIB)
if(WPA_SUPPLICANT_LIB)
  target_compile_options(${WPA_SUPPLICANT_LIB} PRIVATE -Wno-error)
endif()

# --- relax Werror globally (C and C++) ---
idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Wno-error" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error" APPEND)
