# Publishing only. Development keeps the dynamic x64-windows triplet.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# Keep dependencies on the same Visual Studio installation as the game.
if(DEFINED ENV{VSINSTALLDIR} AND EXISTS "$ENV{VSINSTALLDIR}/Common7/IDE")
    get_filename_component(_immune_vs_path "$ENV{VSINSTALLDIR}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_immune_vs_path}" _immune_vs_path)
    set(VCPKG_VISUAL_STUDIO_PATH "${_immune_vs_path}")
endif()
