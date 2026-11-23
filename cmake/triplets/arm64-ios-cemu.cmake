set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)

# Explicit iOS deployment target baseline for physical ARM64 iPadOS
set(VCPKG_OSX_DEPLOYMENT_TARGET "16.0")
set(VCPKG_C_FLAGS "-miphoneos-version-min=16.0")
set(VCPKG_CXX_FLAGS "-miphoneos-version-min=16.0")
set(VCPKG_LINKER_FLAGS "-miphoneos-version-min=16.0")
