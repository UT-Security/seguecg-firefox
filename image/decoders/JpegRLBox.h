/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef JPEG_RLBOX
#define JPEG_RLBOX

#include "JpegRLBoxTypes.h"

// Load general firefox configuration of RLBox
#include "mozilla/rlbox/rlbox_config.h"

#ifdef MOZ_WASM_SANDBOXING_JPEG
// Include the generated header file so that we are able to resolve the symbols
// in the wasm binary
#  include "rlbox.wasm.h"
#  define RLBOX_USE_STATIC_CALLS() rlbox_wasm2c_sandbox_lookup_symbol
#  include "mozilla/rlbox/rlbox_wasm2c_sandbox.hpp"
#else
#  define RLBOX_USE_STATIC_CALLS() rlbox_noop_sandbox_lookup_symbol
#  include "mozilla/rlbox/rlbox_noop_sandbox.hpp"
#endif

#include "mozilla/rlbox/rlbox.hpp"

// Struct info needed for rlbox_load_structs_from_library
extern "C" {
#include "jpeglib.h"
}

#include "JpegStructsForRLBox.h"
rlbox_load_structs_from_library(jpeg);

#endif
