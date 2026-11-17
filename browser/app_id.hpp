// SPDX-License-Identifier: GPL-3.0-or-later
// The webOS app id and launcher title, set by CMake (WPEB_APP_ID, WPEB_APP_TITLE) so a test build can be installed next
// to the normal one under its own id. The runtime folder (/tmp/<id>) follows the id; the engine's loader path must match
// (tools/stage-engine.sh LOADER_LINK).
#pragma once
#ifndef WPEB_APP_ID
#define WPEB_APP_ID "org.sostk.wpebrowser"
#endif
#ifndef WPEB_APP_TITLE
#define WPEB_APP_TITLE "Wren"
#endif
