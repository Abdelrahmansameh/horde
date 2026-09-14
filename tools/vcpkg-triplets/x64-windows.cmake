# Overlay for the stock x64-windows triplet (CMakePresets.json points
# VCPKG_OVERLAY_TRIPLETS here).
#
# WHY: vcpkg picks the NEWEST Visual Studio on the machine to build the
# dependencies, independently of which compiler CMake is using for the game.
# With more than one VS installed that builds Catch2 & co. against a newer
# STL than the one immune links with, and the link fails on missing
# __std_* runtime symbols. Pinning vcpkg to the VS whose vcvars is active
# (VSINSTALLDIR, which vcvars64.bat exports) keeps both halves on the same
# toolset. Outside a developer prompt nothing is pinned and vcpkg behaves as
# stock.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

if(DEFINED ENV{VSINSTALLDIR} AND EXISTS "$ENV{VSINSTALLDIR}/Common7/IDE")
    # vcpkg matches this against vswhere's output verbatim: native separators,
    # no trailing slash.
    get_filename_component(_immune_vs_path "$ENV{VSINSTALLDIR}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_immune_vs_path}" _immune_vs_path)
    set(VCPKG_VISUAL_STUDIO_PATH "${_immune_vs_path}")
endif()
