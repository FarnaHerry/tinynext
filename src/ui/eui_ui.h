// eui_ui.h — eui_neo.h minus the app-entry machinery (eui/detail/dsl_app_impl.h).
//
// dsl_app_impl.h defines inline functions containing lambdas (app::update's
// composeFrame, etc.) whose compiler-generated mangled names collide when the
// header is included in both a plain TU (app.cpp — it must define the
// app::dslAppConfig / app::compose entry points) and module global fragments.
// Our TUs only draw widgets and never call the app-entry machinery, so this
// reduced header is safe to include from app.cpp and every tinynext.ui.* module.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "eui/dsl_app.h"
#include "eui/dsl.h"
#include "eui/image.h"
#include "eui/json.h"
#include "eui/network.h"
#include "eui/platform.h"
#include "eui/signal.h"
#include "eui/types.h"
#include "components/components.h"
