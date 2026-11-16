cmake_minimum_required(VERSION 3.13)
project(runner LANGUAGES C CXX)

set(VIGIL_ROOT "${CMAKE_SOURCE_DIR}/../..")
set(VIGIL_DEPS "${VIGIL_ROOT}/build/host")

configure_file("${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "${CMAKE_CURRENT_BINARY_DIR}/generated_plugin_registrant.c" COPYONLY)

add_executable(${BINARY_NAME}
  "main.c"
  "audio.c"
  "desktop.c"
  "dialog.c"
  "memo.c"
  "store.c"
  "${CMAKE_CURRENT_BINARY_DIR}/generated_plugin_registrant.c"
)

target_compile_options(${BINARY_NAME} PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-std=c99;-D_GNU_SOURCE;-DPLATFORM_HOST;-Wall;-Werror>"
  "$<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes;-Wmissing-declarations>"
  "$<$<COMPILE_LANGUAGE:C>:-Wshadow;-Wpointer-arith;-Wsign-compare>"
  "$<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>")
target_compile_options(${BINARY_NAME} PRIVATE
  "$<$<NOT:$<CONFIG:Debug>>:-O2>")
target_compile_definitions(${BINARY_NAME} PRIVATE
  "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>")

target_include_directories(${BINARY_NAME} PRIVATE
  "${CMAKE_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}" "${VIGIL_ROOT}/include"
  "${VIGIL_DEPS}/include" "${VIGIL_DEPS}/libsodium/include")

pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavutil)

target_link_libraries(${BINARY_NAME} PRIVATE flutter PkgConfig::GTK
  PkgConfig::PULSE PkgConfig::AV
  -Wl,--start-group
  "${VIGIL_ROOT}/libvigil.a"
  "${VIGIL_DEPS}/lib/libkyrka.a"
  "${VIGIL_DEPS}/libsodium/lib/libsodium.a"
  "${VIGIL_DEPS}/opus/lib/libopus.a"
  -Wl,--end-group
  m pthread)
