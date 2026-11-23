project(cemuMain)

option(CEMU_CXX_FLAGS "Additional flags used for compiling Cemu source code")
separate_arguments(CEMU_CXX_FLAGS NATIVE_COMMAND ${CEMU_CXX_FLAGS})
if(CEMU_CXX_FLAGS)
	add_compile_options(${CEMU_CXX_FLAGS})
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message( FATAL_ERROR "Pointers are not 64bit" )
endif()

if(MSVC)
	add_compile_definitions(WIN32_LEAN_AND_MEAN CURL_STATICLIB)
elseif(UNIX)
	if(APPLE)
		if (ENABLE_VULKAN)
			add_compile_definitions(
				_XOPEN_SOURCE
				VK_USE_PLATFORM_MACOS_MVK
				VK_USE_PLATFORM_METAL_EXT
			)
		else()
			add_compile_definitions(_XOPEN_SOURCE)
		endif()
	else()
		if (ENABLE_VULKAN)
			add_compile_definitions(
				VK_USE_PLATFORM_XLIB_KHR # legacy. Do we need to support XLIB surfaces?
				VK_USE_PLATFORM_XCB_KHR
			)
			if (ENABLE_WAYLAND)
				add_compile_definitions(VK_USE_PLATFORM_WAYLAND_KHR)
			endif()
		endif()
	endif()
	# warnings
	if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
		add_compile_options(-Wno-ambiguous-reversed-operator)
	endif()

    add_compile_options(-Wno-multichar -Wno-invalid-offsetof -Wno-switch -Wno-ignored-attributes -Wno-deprecated-enum-enum-conversion)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# on msvc we can reuse the PCH from CemuCommon, on other platforms it's not straight forward to reuse the PCH so we opt out of it
function(cemu_use_precompiled_header target_name)
	if(MSVC)
		target_precompile_headers(${target_name} REUSE_FROM CemuCommon)
	else()
		target_precompile_headers(${target_name} PRIVATE "${CMAKE_SOURCE_DIR}/src/Common/precompiled.h")
	endif()
endfunction()

add_subdirectory(Common)
add_subdirectory(gui)
add_subdirectory(Cafe)
add_subdirectory(Cemu)
add_subdirectory(config)
add_subdirectory(input)
add_subdirectory(audio)
add_subdirectory(util)
add_subdirectory(imgui)
add_subdirectory(resource)
