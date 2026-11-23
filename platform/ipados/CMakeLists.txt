# platform/ipados/CMakeLists.txt
# Defines the CemuIOS native iPadOS application target.
# Only included when CMAKE_SYSTEM_NAME == "iOS".

enable_language(Swift)

set(CEMUIOS_BUNDLE_IDENTIFIER "com.77emul.app" CACHE STRING
    "Bundle identifier for the iOS application")
set(CEMUIOS_VERSION "0.1.0" CACHE STRING "Numeric iOS app version")
set(CEMUIOS_BUILD_NUMBER "1056" CACHE STRING "Monotonically increasing iOS build number")
set(CEMUIOS_RELEASE_NAME "0.1.0 Beta 3" CACHE STRING "User-visible release label")
set(CEMUIOS_DEVELOPMENT_TEAM "" CACHE STRING "Apple team ID used for code signing")
set(CEMUIOS_COVER_MODEL_DOWNLOAD_URL
    "https://github.com/77emul/77U-BETA/releases/download/cover-model-v1/Artwork-normal.77umodel"
    CACHE STRING "HTTPS URL for the verified standard cover model")
string(REPLACE "&" "&amp;" CEMUIOS_COVER_MODEL_URL_XML "${CEMUIOS_COVER_MODEL_DOWNLOAD_URL}")
string(REPLACE "<" "&lt;" CEMUIOS_COVER_MODEL_URL_XML "${CEMUIOS_COVER_MODEL_URL_XML}")
string(REPLACE ">" "&gt;" CEMUIOS_COVER_MODEL_URL_XML "${CEMUIOS_COVER_MODEL_URL_XML}")

set(CEMUIOS_SWIFT_SOURCES
    App/ArtworkModelArchive.swift
    App/GameBootArtwork.swift
    App/MenuTheme.swift
    App/MenuControllerNavigation.swift
    App/StartupSound.swift
    App/CemuIOSApp.swift
    App/ContentView.swift
    App/AppSettingsView.swift
    App/SaveManagementView.swift
    App/SaveExportLocation.swift
    App/GameArtworkEnhancer.swift
    App/ArtworkEnhancementLifecycle.swift
    App/GraphicsPacksView.swift
    App/WelcomeProgress.swift
    App/WelcomeGuideView.swift
    Views/DiagnosticsActionsView.swift
    Views/LibraryExperience.swift
    Views/MetalView.swift
    Views/TouchController.swift
    Views/PlayOverlay.swift
    Views/ExternalDisplay.swift
    Views/GameCoverFlowView.swift
    Views/GameLibraryGridView.swift
    Views/LibraryGameDock.swift
    Views/LibraryLayoutTransition.swift
    Views/LibraryLaunchScreen.swift
    Views/GameCoverFlowGeometry.swift
)

set(CEMUIOS_OBJCXX_SOURCES
    Views/EmulatorMetalView.mm
    Bridge/CemuBridge.mm
    Bridge/SaveManagerIOS.mm
    Bridge/WindowSystemIOS.mm
    Bridge/JIT26Support.mm
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteTexture.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteTextureLegacy.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteTextureView.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/FetchShader.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteTextureLoader.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteIndices.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteBufferCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteTextureCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LattePerformanceMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteAsyncCommands.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteThread.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteTiming.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteSurfaceCopy.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteQuery.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteStreamoutGPU.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteTextureReadback.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteRingBuffer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteShader.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteShaderCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteOverlay.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteSoftware.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Transcompiler/LatteTC.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Transcompiler/LatteTCGenIR.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSL.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSLAttrDecoder.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerRegisterDataTypeTracker.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/ShaderInfo/ShaderDescription.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/ShaderInfo/ShaderInstanceInfo.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/RendererCore.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/RendererShader.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LatteAddrLib/LatteAddrLib_Coord.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteBufferData.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Core/LatteGSCopyShaderParser.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitMSL.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitMSLAttrDecoder.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/GraphicPack/GraphicPack2.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/GraphicPack/GraphicPack2Patches.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/GraphicPack/GraphicPack2PatchesApply.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/GraphicPack/GraphicPack2PatchesParser.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/fscDeviceRedirect.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/FST/KeyCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/WUD/wud.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/WUHB/WUHBReader.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/fscDeviceWud.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/fscDeviceWua.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/fscDeviceWuhb.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Debugger/DebugSymbolStorage.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Debugger/Debugger.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Debugger/GDBStub.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Debugger/GDBBreakpoints.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Common/RegisterSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Common/ShaderSerializer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/MMU/MMU.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Interpreter/PPCInterpreterMain.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Interpreter/PPCInterpreterImpl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Interpreter/PPCInterpreterOPC.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Interpreter/PPCInterpreterFPU.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Interpreter/PPCInterpreterPS.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Interpreter/PPCInterpreterHLE.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/PPCTimer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/PPCScheduler.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/IML/IMLSegment.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/IML/IMLInstruction.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/IML/IMLDebug.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/IML/IMLAnalyzer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/IML/IMLOptimizer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/IML/IMLRegisterAllocator.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/IML/IMLRegisterAllocatorRanges.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGen.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGenFPU.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/PPCRecompilerIntermediate.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Espresso/Recompiler/BackendAArch64/BackendAArch64.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/common/OSCommon.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/RPL/rpl_symbol_storage.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/RPL/rpl_trampoline.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/RPL/rpl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/RPL/elf.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/RPL/COSModule.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/RPL/rpl_debug_symbols.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/CafeSystem.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Init.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Alarm.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Atomic.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_BSP.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Callbacks.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_CodeGen.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Coroutine.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_DynLoad.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_FG.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_FS.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_FSInit.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_GHS.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_HWInterface.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_IM.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_IOS.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_IPC.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_IPCBuf.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_LockedCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MCP.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MEM.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MEM_BlockHeap.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MEM_FrmHeap.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MEM_UnitHeap.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Memory.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MemoryMapping.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MessageQueue.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Misc.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_MPQueue.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_OSScreen.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_OverlayArena.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Scheduler.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Spinlock.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Synchronization.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_SysArea.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_SysHeap.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_SystemInfo.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_ThreadQueue.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_ThreadState.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/coreinit/coreinit_Time.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Account/Account.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/proc_ui/proc_ui.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/padscore/padscore.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/vpad/vpad.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/swkbd/swkbd.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/erreula/erreula.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_core/ax_aux.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_core/ax_exports.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_core/ax_ist.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_core/ax_mix.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_core/ax_multivoice.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_core/ax_out.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_core/ax_voice.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_AddrTest.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Blit.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Command.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_ContextState.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Draw.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Event.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Memory.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Misc.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Query.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_RenderTarget.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Resource.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Shader.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_shader_legacy.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_State.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Streamout.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Surface.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Surface_Copy.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_Texture.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/gx2/GX2_TilingAperture.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/TCL/TCL.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/mic/mic.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/zlib125/zlib125.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsysnet/nsysnet.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nlibcurl/nlibcurl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/snd_user/snd_user.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_ac/nn_ac.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_boss/nn_boss.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_ndm/nn_ndm.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/avm/avm.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv_Common.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv_InitializeTypes.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv_DownloadCommunityTypes.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv_UploadCommunityTypes.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv_UploadFavoriteTypes.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv_PostTypes.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_olv/nn_olv_OfflineDB.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_aoc/nn_aoc.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_ec/nn_ec.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_uds/nn_uds.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_nim/nn_nim.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_spm/nn_spm.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_sl/nn_sl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_temp/nn_temp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_pdm/nn_pdm.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_idbe/nn_idbe.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_cmpt/nn_cmpt.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_ccr/nn_ccr.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/dmae/dmae.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/drmapp/drmapp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/camera/camera.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/nsyshid.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/AttachDefaultBackends.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/Whitelist.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/BackendEmulated.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/Dimensions.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/Infinity.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/Skylander.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/SkylanderXbox360.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyshid/g721/g721.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nsyskbd/nsyskbd.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nlibnss/nlibnss.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/h264_avc/H264Dec.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/h264_avc/H264DecBackendAVC.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/h264_avc/parser/H264Parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_save/nn_save.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_act/nn_act.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_acp/nn_acp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_fp/nn_fp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/sysapp/sysapp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nn_nfp/nn_nfp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nfc/nfc.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nfc/ndef.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nfc/stream.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nfc/TagV0.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/nfc/TLV.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/OS/libs/ntag/ntag.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/GamePatch.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/GameProfile/GameProfile.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/TitleList/GameInfo.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/TitleList/TitleInfo.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/TitleList/TitleList.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/TitleList/SaveInfo.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/TitleList/SaveList.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/FST/FST.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/SI/SI.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/VI/VI.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/kernel/iosu_kernel.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/fsa/iosu_fsa.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/ccr_nfc/iosu_ccr_nfc.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/legacy/iosu_acp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/legacy/iosu_act.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/legacy/iosu_crypto.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/legacy/iosu_fpd.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/legacy/iosu_ioctl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/legacy/iosu_mcp.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/legacy/iosu_nim.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/nn/iosu_nn_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/nn/boss/boss_common.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/nn/boss/boss_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/PDM/iosu_pdm.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/IOSU/ODM/iosu_odm.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/fsc.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/Filesystem/fscDeviceHostFS.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Renderer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalFrameGeneration.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalLayer.mm"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalLayerHandle.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalDepthStencilCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalSamplerCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalOutputShaderCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalQuery.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/CachedFBOMtl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/LatteToMtl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalAttachmentsInfo.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Metal/MetalCppImpl.cpp"
    "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/RendererOuputShader.cpp"
    "${CMAKE_SOURCE_DIR}/src/imgui/imgui_extension.cpp"
    "${CMAKE_SOURCE_DIR}/src/imgui/imgui_impl_metal.mm"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_widgets.cpp"
)

set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/src/imgui/imgui_impl_metal.mm"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui/imgui_widgets.cpp"
    PROPERTIES COMPILE_FLAGS "-Wno-nontrivial-memcall -Wno-invalid-offsetof"
)

set(CEMUIOS_HEADERS
    Views/EmulatorMetalView.h
    Bridge/CemuBridge.h
    Bridge/CemuIOS-Bridging-Header.h
)

set(CEMUIOS_RESOURCES
    77U.icon
)

# Cemu's redistributable Noto CJK replacements for the Wii U shared fonts.
# Games such as Splatoon request these through OSGetSharedData for dynamic UI
# labels. Preserve the path expected by CafeSystem::LoadSharedData().
file(GLOB CEMUIOS_SHARED_FONT_FILES
    "${CMAKE_SOURCE_DIR}/bin/resources/sharedFonts/*.ttf"
)
foreach(SHARED_FONT_FILE IN LISTS CEMUIOS_SHARED_FONT_FILES)
    list(APPEND CEMUIOS_RESOURCES "${SHARED_FONT_FILE}")
    set_source_files_properties("${SHARED_FONT_FILE}" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION "Resources/CemuData/resources/sharedFonts"
    )
endforeach()

# Default game profiles for known titles (e.g. Paper Mario Color Splash, Mario Kart 8, etc.)
file(GLOB CEMUIOS_DEFAULT_GAME_PROFILES
    "${CMAKE_SOURCE_DIR}/bin/gameProfiles/default/*.ini"
)
foreach(PROFILE_FILE IN LISTS CEMUIOS_DEFAULT_GAME_PROFILES)
    list(APPEND CEMUIOS_RESOURCES "${PROFILE_FILE}")
    set_source_files_properties("${PROFILE_FILE}" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION "Resources/CemuData/gameProfiles/default"
    )
endforeach()

set(CEMUIOS_GENERATED_LICENSE_DIR "${CMAKE_CURRENT_BINARY_DIR}/GeneratedLicenses")
file(MAKE_DIRECTORY "${CEMUIOS_GENERATED_LICENSE_DIR}")
set(CEMUIOS_THIRD_PARTY_NOTICES "${CEMUIOS_GENERATED_LICENSE_DIR}/ThirdParty-Notices.txt")
file(WRITE "${CEMUIOS_THIRD_PARTY_NOTICES}"
    "77U third-party dependency notices\n\nCollected from the dependencies installed for this build. Some listed components may not be linked into the app.\n")
file(GLOB VCPKG_COPYRIGHT_FILES
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright"
)
foreach(COPYRIGHT_FILE IN LISTS VCPKG_COPYRIGHT_FILES)
    get_filename_component(COPYRIGHT_DIR "${COPYRIGHT_FILE}" DIRECTORY)
    get_filename_component(COPYRIGHT_COMPONENT "${COPYRIGHT_DIR}" NAME)
    file(READ "${COPYRIGHT_FILE}" COPYRIGHT_TEXT)
    file(APPEND "${CEMUIOS_THIRD_PARTY_NOTICES}"
        "\n\n===== ${COPYRIGHT_COMPONENT} =====\n\n${COPYRIGHT_TEXT}")
endforeach()
file(READ "${CMAKE_SOURCE_DIR}/dependencies/ih264d/NOTICE" IH264D_NOTICE)
file(APPEND "${CEMUIOS_THIRD_PARTY_NOTICES}"
    "\n\n===== ih264d =====\n\n${IH264D_NOTICE}")

file(GLOB CEMUIOS_LICENSE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Licenses/*.txt"
)
list(APPEND CEMUIOS_LICENSE_FILES "${CEMUIOS_THIRD_PARTY_NOTICES}")
foreach(LICENSE_FILE IN LISTS CEMUIOS_LICENSE_FILES)
    list(APPEND CEMUIOS_RESOURCES "${LICENSE_FILE}")
    set_source_files_properties("${LICENSE_FILE}" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION "Resources/Licenses"
    )
endforeach()

# Ship FPS++ so a fresh installation can run BOTW with its 60 FPS default
# without downloading the community packs first. Keep every assembly file and
# the rules together; the loader scans graphicPacks in the application bundle.
file(GLOB_RECURSE CEMUIOS_BOTW_FPS_PACK_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/graphicPacks/BreathOfTheWild/Mods/FPS++/*"
)
foreach(PACK_FILE IN LISTS CEMUIOS_BOTW_FPS_PACK_FILES)
    file(RELATIVE_PATH PACK_RELATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Resources" "${PACK_FILE}")
    get_filename_component(PACK_RESOURCE_DIR "${PACK_RELATIVE_PATH}" DIRECTORY)
    list(APPEND CEMUIOS_RESOURCES "${PACK_FILE}")
    set_source_files_properties("${PACK_FILE}" PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION "Resources/${PACK_RESOURCE_DIR}"
    )
endforeach()
# Other game-specific packs remain available from the in-app pack browser.

add_executable(CemuIOS MACOSX_BUNDLE
    ${CEMUIOS_SWIFT_SOURCES}
    ${CEMUIOS_OBJCXX_SOURCES}
    ${CEMUIOS_HEADERS}
    ${CEMUIOS_RESOURCES}
)

# --- Optional Metal frame capture ---
# Enabling capture installs Apple's Metal API wrappers even when no capture is
# recording. Keep normal gameplay builds uninstrumented. Detailed logs use CPU
# clocks and completed-command-buffer timestamps and do not need this layer.
option(CEMUIOS_ENABLE_METAL_CAPTURE "Enable Metal frame capture instrumentation for diagnostic builds" OFF)
if(CEMUIOS_ENABLE_METAL_CAPTURE)
    set(CEMUIOS_METAL_CAPTURE_PLIST_VALUE "true")
else()
    set(CEMUIOS_METAL_CAPTURE_PLIST_VALUE "false")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/CemuIOS-Info.plist.in" @ONLY)

# --- Bundle configuration ---
set_target_properties(CemuIOS PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/CemuIOS-Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${CEMUIOS_BUNDLE_IDENTIFIER}"
    MACOSX_BUNDLE_BUNDLE_NAME "77U"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${CEMUIOS_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${CEMUIOS_BUILD_NUMBER}"
)

# --- Asset catalog ---
set_source_files_properties(77U.icon PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
# --- Xcode build settings ---
set_target_properties(CemuIOS PROPERTIES
    # Universal iPhone and iPad app
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    # Bundle identifier
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${CEMUIOS_BUNDLE_IDENTIFIER}"
    # Code signing
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CEMUIOS_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/CemuIOS.entitlements"
    # Swift bridging header
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/CemuIOS-Bridging-Header.h"
    # Swift version
    XCODE_ATTRIBUTE_SWIFT_VERSION "6.0"
    # CMake's default per-configuration Swift SDK path does not exist for
    # RelWithDebInfo. Use the SDK's stable Swift library directory instead.
    XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS "$(inherited) $(SDKROOT)/usr/lib/swift"
    # App icon
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "77U"
    # Enable debug dylib for debug builds
    XCODE_ATTRIBUTE_ENABLE_DEBUG_DYLIB[variant=Debug] "YES"
    # Generate proper dsym for debugging
    XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT[variant=Debug] "dwarf-with-dsym"
    # Product name
    XCODE_ATTRIBUTE_PRODUCT_NAME "CemuIOS"
    # Obj-C++ ARC
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
)

# Device play needs an optimized emulator even when attaching StikDebug for JIT.
# Generate an explicit scheme so Xcode does not silently default to -O0 Debug.
set_target_properties(CemuIOS PROPERTIES
    XCODE_GENERATE_SCHEME TRUE
    XCODE_SCHEME_LAUNCH_CONFIGURATION "RelWithDebInfo"
    XCODE_SCHEME_PROFILE_CONFIGURATION "RelWithDebInfo"
    XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL[variant=Release] "3"
    XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL[variant=RelWithDebInfo] "3"
    XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL[variant=Release] "-O"
    XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL[variant=RelWithDebInfo] "-O"
)

# --- Include paths for Obj-C++ sources and Cemu headers ---
target_include_directories(CemuIOS PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/Common"
    "${CMAKE_SOURCE_DIR}/src/Cafe"
    "${CMAKE_SOURCE_DIR}/src/gui/interface"
    "${CMAKE_SOURCE_DIR}/dependencies/metal-cpp"
    "${CMAKE_SOURCE_DIR}/dependencies/imgui"
    "${CMAKE_SOURCE_DIR}/dependencies/xbyak_aarch64"
    "${CMAKE_SOURCE_DIR}/dependencies/xbyak_aarch64/xbyak_aarch64"
    "${CMAKE_CURRENT_SOURCE_DIR}/Bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/Views"
)

target_compile_definitions(CemuIOS PRIVATE
    ENABLE_METAL=1
    IMGUI_IMPL_METAL_CPP=1
)

# ImGui is bundled third-party code and still uses its documented zeroed POD-
# style storage internally. Do not hide this diagnostic from Cemu core code.
target_compile_options(CemuIOS PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-nontrivial-memcall>"
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-nontrivial-memcall>"
)

target_precompile_headers(CemuIOS REUSE_FROM CemuCommon)

# --- Libraries & frameworks ---
# Note: OpenSSL::SSL transitively links OpenSSL::Crypto; omitting explicit OpenSSL::Crypto avoids duplicate libcrypto.a
target_link_libraries(CemuIOS PRIVATE
    CemuCommon
    CemuUtil
    CemuComponents
    CemuConfig
    CemuInput
    CemuAudio
    CemuResource
    xbyak_aarch64
    ih264d
    ZArchive::zarchive
    libzip::zip
    CURL::libcurl
    ZLIB::ZLIB
    zstd::zstd
    OpenSSL::SSL
    pugixml::pugixml
    fmt::fmt
    "-framework UIKit"
    "-framework CoreML"
    "-framework Metal"
    "-framework MetalFX"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework GameController"
    "-framework CoreMotion"
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework AVFoundation"
)

if (ENABLE_SDL)
    target_link_libraries(CemuIOS PRIVATE SDL3::SDL3)
endif()

target_link_options(CemuIOS PRIVATE "-Wl,-no_warn_duplicate_libraries")

# Embedded JIT acquisition for the GitHub/sideload build.
add_subdirectory(JIT)
add_subdirectory(Memory)

# Keep matching symbols for crashes in the two extension processes.
set_target_properties(CemuJITHelper CemuSharedRAMHelper PROPERTIES
    XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS[variant=Release] YES
    XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT[variant=Release] "dwarf-with-dsym"
)

# Downloaded source trees can carry macOS provenance/quarantine
# metadata into copied bundle resources. codesign may then misclassify normal
# assets (for example Assets.car or app icons) as unsigned nested code. Strip
# only extended metadata from the generated product immediately before Xcode's
# CodeSign phase. This never removes files or changes user data.
add_custom_command(TARGET CemuIOS POST_BUILD
    COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/PrepareCodeSigning.sh"
    COMMENT "Preparing CemuIOS bundle for code signing"
    VERBATIM
)
