cmake_minimum_required(VERSION 3.21.1)

option(ENABLE_VCPKG "Enable the vcpkg package manager" ON)

# used by CI script to set version:
set(EMULATOR_VERSION_MAJOR "0" CACHE STRING "")
set(EMULATOR_VERSION_MINOR "0" CACHE STRING "")
set(EMULATOR_VERSION_PATCH "0" CACHE STRING "")

set(THREADS_PREFER_PTHREAD_FLAG true) # needs to be set before vcpkg

execute_process(
		COMMAND git log --format=%h -1
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
)
if(NOT GIT_HASH)
	set(GIT_HASH snapshot)
endif()
add_definitions(-DEMULATOR_HASH=${GIT_HASH})

if (ENABLE_VCPKG)
	# check if vcpkg is shallow and unshallow it if necessary
	execute_process(
	COMMAND git rev-parse --is-shallow-repository
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/vcpkg
	OUTPUT_VARIABLE is_vcpkg_shallow
	OUTPUT_STRIP_TRAILING_WHITESPACE
    )

	if(is_vcpkg_shallow STREQUAL "true")
		message(STATUS "vcpkg is shallow. Unshallowing it now...")
		execute_process(
			COMMAND git fetch --unshallow
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dependencies/vcpkg"
			RESULT_VARIABLE result
			OUTPUT_VARIABLE output
		)
	endif()

	set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_mac;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake"
		CACHE STRING "Vcpkg toolchain file")
	# Set this so that all the various find_package() calls don't need an explicit
	# CONFIG option
	set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
	list(APPEND VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/triplets")
	set(VCPKG_TARGET_TRIPLET "arm64-ios-cemu" CACHE STRING "")
endif()

project(Cemu VERSION 2.0.0)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
	message(FATAL_ERROR "77U builds for iOS only. Configure with -DCMAKE_SYSTEM_NAME=iOS (see BUILD.md).")
endif()

if(APPLE)
    foreach(language C CXX OBJC OBJCXX)
        string(APPEND CMAKE_${language}_FLAGS " -ffile-prefix-map=\"${CMAKE_SOURCE_DIR}=.\" -ffile-prefix-map=\"${CMAKE_BINARY_DIR}=./build\"")
    endforeach()
endif()

enable_language(ASM)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions($<$<CONFIG:Debug>:CEMU_DEBUG_ASSERT>) # if build type is debug, set CEMU_DEBUG_ASSERT

add_definitions(-DEMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR})
add_definitions(-DEMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR})
add_definitions(-DEMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# enable link time optimization for release builds
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY XCODE_STARTUP_PROJECT CemuIOS)
endif()

enable_language(OBJC OBJCXX)
# The app uses the Metal 4 command submission model exclusively.
set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum iOS deployment version" FORCE)

option(ENABLE_OPENGL "Enables the OpenGL backend" OFF)
option(ENABLE_VULKAN "Enables the Vulkan backend" OFF)
option(ENABLE_METAL "Enables the Metal backend" ON)
option(ENABLE_DISCORD_RPC "Enables the Discord Rich Presence feature" OFF)

# input backends
option(ENABLE_HIDAPI "Build with HIDAPI" OFF)
option(ENABLE_SDL "Enables the SDLController backend" OFF)
option(ENABLE_LIBUSB "Build with libusb support" OFF)

if (ENABLE_LIBUSB)
	add_compile_definitions(HAS_LIBUSB)
endif()

# audio backends
option(ENABLE_CUBEB "Enabled cubeb backend" ON)


find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(pugixml REQUIRED)
find_package(Boost COMPONENTS program_options filesystem nowide REQUIRED)
find_package(libzip REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd MODULE REQUIRED) # MODULE so that zstd::zstd is available
find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
find_package(glm REQUIRED)
find_package(fmt 12.1 REQUIRED)
find_package(PNG REQUIRED)

if(ENABLE_SDL)
	find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
	add_compile_definitions(HAS_SDL)
endif()

if (ENABLE_VULKAN)
	find_package(glslang REQUIRED)
	# glslang versions older than 11.11.0 define targets without a namespace
	if (NOT TARGET glslang::SPIRV AND TARGET SPIRV)
		add_library(glslang::SPIRV ALIAS SPIRV)
	endif()
	include_directories("dependencies/Vulkan-Headers/include")
	add_compile_definitions(ENABLE_VULKAN)
	add_compile_definitions(VK_NO_PROTOTYPES)
endif()

if (ENABLE_OPENGL)
	find_package(OpenGL REQUIRED)
	add_compile_definitions(ENABLE_OPENGL)
endif()

if (ENABLE_METAL)
    include_directories(${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)

    add_compile_definitions(ENABLE_METAL)
endif()

if (ENABLE_DISCORD_RPC)
	add_compile_definitions(ENABLE_DISCORD_RPC)
endif()

if (ENABLE_HIDAPI)
	set(SUPPORTS_WIIMOTE ON)
	add_compile_definitions(HAS_HIDAPI)
endif ()

if (ENABLE_CUBEB)
	if (NOT ENABLE_VCPKG)
	    find_package(cubeb)
	endif()
	if (NOT cubeb_FOUND)
		option(BUILD_TESTS "" OFF)
		option(BUILD_TOOLS "" OFF)
		option(BUNDLE_SPEEX "" OFF)
		set(USE_WINMM OFF CACHE BOOL "")
		if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
			find_package(Git QUIET)
			if (GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/cubeb.patch")
				execute_process(
					COMMAND ${GIT_EXECUTABLE} apply --check "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/cubeb.patch"
					WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/cubeb"
					RESULT_VARIABLE _cubeb_patch_check
					OUTPUT_QUIET ERROR_QUIET
				)
				if (_cubeb_patch_check EQUAL 0)
					message(STATUS "Applying cubeb iOS patch from cmake/patches/cubeb.patch")
					execute_process(
						COMMAND ${GIT_EXECUTABLE} apply "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/cubeb.patch"
						WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/cubeb"
						RESULT_VARIABLE _cubeb_patch_result
					)
					if (NOT _cubeb_patch_result EQUAL 0)
						message(FATAL_ERROR "Failed to apply cubeb iOS patch")
					endif()
				endif()
			endif()
		endif()
		add_subdirectory("dependencies/cubeb" EXCLUDE_FROM_ALL)
		set_property(TARGET cubeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		# cubeb is externally maintained and intentionally uses fixed-width audio
		# frame counters throughout its resampler. Keep its Apple/Clang diagnostics
		# target-local so Cemu's own narrowing warnings remain visible.
		if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
			target_compile_options(cubeb PRIVATE
				-Wno-shorten-64-to-32
				-Wno-unused-function
				-Wno-unused-but-set-variable
			)
		endif()
		add_library(cubeb::cubeb ALIAS cubeb)
	endif()
	add_compile_definitions("HAS_CUBEB=1")
endif()

add_subdirectory("dependencies/ih264d" EXCLUDE_FROM_ALL)

# Keep warnings from bundled, externally maintained libraries out of the iOS
# app's issue list. These flags are target-local and do not change generated
# code or relax diagnostics for Cemu sources.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	target_compile_options(ih264d PRIVATE
		-Wno-shorten-64-to-32
		-Wno-deprecated-declarations
	)
endif()

if (CMAKE_OSX_ARCHITECTURES)
	set(CEMU_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
else()
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	add_subdirectory("dependencies/xbyak_aarch64" EXCLUDE_FROM_ALL)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(xbyak_aarch64 PRIVATE
			-Wno-shorten-64-to-32
			-Wno-cast-qual
		)
	endif()
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
	add_library(zarchive STATIC
		"dependencies/ZArchive/src/sha_256.c"
		"dependencies/ZArchive/src/zarchivereader.cpp"
		"dependencies/ZArchive/src/zarchivewriter.cpp"
	)
	target_include_directories(zarchive PUBLIC "dependencies/ZArchive/include")
	target_link_libraries(zarchive PRIVATE zstd::zstd)
	target_compile_options(zarchive PRIVATE -Wno-shorten-64-to-32)
	add_library(ZArchive::zarchive ALIAS zarchive)
else()
	find_package(ZArchive QUIET)
	if (NOT ZArchive_FOUND)
		add_subdirectory("dependencies/ZArchive" EXCLUDE_FROM_ALL)
	endif()
endif()

add_subdirectory(src)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
	# These layouts mirror guest objects or compact tagged instruction payloads;
	# their byte offsets and zeroed storage are validated by Cemu assertions.
	target_compile_options(CemuCafe PRIVATE
		-Wno-invalid-offsetof
	)
	target_compile_options(CemuUtil PRIVATE
		-Wno-invalid-offsetof
	)

	add_subdirectory("platform/ipados")
endif()
