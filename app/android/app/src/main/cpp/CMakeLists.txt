# The vigil core and its Android glue as libvigil.so, linked against
# what dist-build/android-build.sh built.

cmake_minimum_required(VERSION 3.22)
project(vigil C)

if(NOT VIGIL_ROOT)
	message(FATAL_ERROR "VIGIL_ROOT is not set")
endif()

set(DEPS ${VIGIL_ROOT}/build/android-${ANDROID_ABI})

if(NOT EXISTS ${DEPS}/lib/libkyrka.a)
	message(FATAL_ERROR "no libkyrka for ${ANDROID_ABI}, "
	    "run dist-build/android-build.sh in ${VIGIL_ROOT} first")
endif()

if(NOT EXISTS ${VIGIL_ROOT}/obj/version.c)
	message(FATAL_ERROR "no version.c, "
	    "run dist-build/android-build.sh in ${VIGIL_ROOT} first")
endif()

add_library(vigil SHARED
	${VIGIL_ROOT}/src/vigil.c
	${VIGIL_ROOT}/src/buf.c
	${VIGIL_ROOT}/src/call.c
	${VIGIL_ROOT}/src/chat.c
	${VIGIL_ROOT}/src/config.c
	${VIGIL_ROOT}/src/ether.c
	${VIGIL_ROOT}/src/grace.c
	${VIGIL_ROOT}/src/inet.c
	${VIGIL_ROOT}/src/io.c
	${VIGIL_ROOT}/src/link.c
	${VIGIL_ROOT}/src/liturgy.c
	${VIGIL_ROOT}/src/peer.c
	${VIGIL_ROOT}/src/remembrance.c
	${VIGIL_ROOT}/src/ring.c
	${VIGIL_ROOT}/src/status.c
	${VIGIL_ROOT}/src/video.c
	${VIGIL_ROOT}/src/voice.c
	${VIGIL_ROOT}/src/platform_android.c
	${VIGIL_ROOT}/src/audio_android.c
	${VIGIL_ROOT}/src/video_android.c
	${VIGIL_ROOT}/obj/version.c)

target_include_directories(vigil PRIVATE
	${VIGIL_ROOT}/include
	${DEPS}/include
	${DEPS}/opus/include
	${DEPS}/libsodium/include)

target_compile_definitions(vigil PRIVATE PLATFORM_ANDROID _GNU_SOURCE=1)

target_compile_options(vigil PRIVATE
	-std=c99 -pedantic -Wall -Werror -Wstrict-prototypes
	-Wmissing-prototypes -Wmissing-declarations -Wshadow
	-Wpointer-arith -Wcast-qual -Wsign-compare -O2
	-fstack-protector-all -Wtype-limits -fno-common)

target_link_libraries(vigil
	${DEPS}/lib/libkyrka.a
	${DEPS}/mbedtls/lib/libmbedcrypto.a
	${DEPS}/libsodium/lib/libsodium.a
	${DEPS}/opus/lib/libopus.a
	aaudio
	log
	m)

target_link_options(vigil PRIVATE "-Wl,-z,max-page-size=16384")
