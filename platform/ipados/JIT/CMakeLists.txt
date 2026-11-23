# Sideload-only StikJIT helper. Private extension APIs require development signing.
set(STIKJIT_ROOT "${CMAKE_SOURCE_DIR}/dependencies/StikJIT")
set(CEMUIOS_IDEVICE_LIBRARY "${STIKJIT_ROOT}/idevice/libidevice_ffi.a" CACHE FILEPATH
    "Path to the arm64 iOS idevice FFI static library")
if(NOT EXISTS "${CEMUIOS_IDEVICE_LIBRARY}")
    message(FATAL_ERROR "idevice FFI is missing. Build it using BUILD.md and set CEMUIOS_IDEVICE_LIBRARY.")
endif()
file(GLOB STIKJIT_SWIFT CONFIGURE_DEPENDS "${STIKJIT_ROOT}/Sources/*.swift")
file(GLOB JIT_LICENSES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Licenses/*.txt")
set_source_files_properties(${JIT_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_sources(CemuIOS PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/JITCoordinator.swift" "${CMAKE_CURRENT_SOURCE_DIR}/JITIPC.swift" "${CMAKE_CURRENT_SOURCE_DIR}/JITRunnerClient.swift" "${CMAKE_CURRENT_SOURCE_DIR}/JITSettingsView.swift" "${CMAKE_CURRENT_SOURCE_DIR}/CemuJITLauncher.mm" ${JIT_LICENSES})
set_source_files_properties("${STIKJIT_ROOT}/Resources/universal.js" "${STIKJIT_ROOT}/Resources/legacy.js" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
add_library(CemuJITHelper MODULE ${STIKJIT_SWIFT} JITIPC.swift Helper/CemuJITRequestHandler.swift Helper/Bootstrap.mm
    "${STIKJIT_ROOT}/Resources/universal.js" "${STIKJIT_ROOT}/Resources/legacy.js")
set_target_properties(CemuJITHelper PROPERTIES
    BUNDLE TRUE BUNDLE_EXTENSION appex PREFIX "" SUFFIX ""
    XCODE_PRODUCT_TYPE "com.apple.product-type.app-extension"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Helper/Info.plist"
    XCODE_ATTRIBUTE_INFOPLIST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Helper/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${CEMUIOS_BUNDLE_IDENTIFIER}.JITHelper"
    XCODE_ATTRIBUTE_PRODUCT_NAME "CemuJITHelper"
    XCODE_ATTRIBUTE_MARKETING_VERSION "${CEMUIOS_VERSION}"
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "${CEMUIOS_BUILD_NUMBER}"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    XCODE_ATTRIBUTE_BUILD_LIBRARY_FOR_DISTRIBUTION YES
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.4"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CEMUIOS_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_APPLICATION_EXTENSION_API_ONLY YES
    XCODE_ATTRIBUTE_SKIP_INSTALL YES
    XCODE_ATTRIBUTE_SWIFT_INCLUDE_PATHS "${STIKJIT_ROOT}/idevice ${STIKJIT_ROOT}/iokit"
    XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS "$(inherited) $(SDKROOT)/usr/lib/swift"
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks @executable_path/../../Frameworks"
    XCODE_ATTRIBUTE_OTHER_LDFLAGS "-e _NSExtensionMain -force_load \"${CEMUIOS_IDEVICE_LIBRARY}\" -Wl,-no_warn_duplicate_libraries"
)
target_link_libraries(CemuJITHelper PRIVATE "-framework Foundation" "-framework JavaScriptCore" "-framework Network" "-framework Security" "-framework CFNetwork" "-framework SystemConfiguration" "-framework IOKit")
add_dependencies(CemuIOS CemuJITHelper)
set_target_properties(CemuIOS PROPERTIES XCODE_EMBED_APP_EXTENSIONS CemuJITHelper XCODE_EMBED_APP_EXTENSIONS_CODE_SIGN_ON_COPY YES)
