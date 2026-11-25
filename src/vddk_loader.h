// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// The host's half of binding the VDDK: finding and loading the library
//
#pragma once

#include <string>

namespace Vddk::Loader
{
    // What the library is called on this host, for the messages.
    const char* libraryName();

    // Loads the library: `file` as given when there is one, else from
    // `directory` when one is given, and through the loader's own search path
    // otherwise. On failure `reason` says why, in the host's words.
    bool open(const std::string& directory, const std::string& file, std::string& reason);

    // An exported symbol of the loaded library; null when it has none by that
    // name.
    void* symbol(const char* name);

    // The directory the library was actually loaded from; empty when the
    // loader declines to say.
    std::string origin();

    // One line on where an installation is looked for, to follow a failed load.
    const char* searchHint();

    void close();
}
