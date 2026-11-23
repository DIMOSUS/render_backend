// Windows SDK include for the backend sources: windows.h without the min/max macros and rarely used parts. Public
// headers under include/render_backend/gpu must not include this file.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h> // IUnknown, used by the DXGI adapter interfaces.
