// RENDER_BACKEND_RUNTIME_CHECKS enables extra contract checks. Defaults to on in debug builds.
#pragma once

#ifndef RENDER_BACKEND_RUNTIME_CHECKS
#ifdef _DEBUG
#define RENDER_BACKEND_RUNTIME_CHECKS 1
#else
#define RENDER_BACKEND_RUNTIME_CHECKS 0
#endif
#endif
