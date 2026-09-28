// gui/icons/NanoSvg.h — includes nanosvg's declarations (vcpkg port `nanosvg`).
// The implementation is compiled once, in NanoSvgImpl.cpp, with warnings off:
// it is third-party C written for a laxer warning level than this project's.
#pragma once

#include <cstdio>   // nanosvg.h declares nsvgParseFromFile with FILE*

#include <nanosvg.h>
#include <nanosvgrast.h>
