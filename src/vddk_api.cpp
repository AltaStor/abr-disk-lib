// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Runtime binding of the VMware VDDK
//
#include "vddk_api.h"

#include <filesystem>
#include <iostream>

#include "vddk_loader.h"

namespace {

Vddk::Api g_api;
bool g_loaded = false;

// The loader hands back a void*, which no standard conversion turns into a
// function pointer; reinterpret_cast is the spelling POSIX settles on. The
// destination type comes from the caller, so nothing is being asserted about
// the signature here beyond what vixDiskLib.h already declares.
template <typename Fn>
bool bindEntryPoint(Fn& entryPoint, const char* symbol)
{
    entryPoint = reinterpret_cast<Fn>(Vddk::Loader::symbol(symbol));
    if (!entryPoint) {
        std::cerr << "Error: " << Vddk::Loader::libraryName() << " does not export '" << symbol << "'\n";
        return false;
    }
    return true;
}

// The bundle's own directory: VixDiskLib_Init needs the installation root so it
// can find the transport plugins.
std::string g_installRoot;

void rememberInstallRoot()
{
    const std::string origin = Vddk::Loader::origin();
    if (origin.empty()) {
        return;
    }

    // The libraries live in lib64 of the installation, or in bin on Windows;
    // VixDiskLib_Init wants the directory above it. A layout that keeps them in
    // the root is taken as-is.
    std::filesystem::path directory(origin);
    const std::string leaf = directory.filename().string();

    g_installRoot = (leaf == "lib64" || leaf == "lib32" || leaf == "bin")
                        ? directory.parent_path().string()
                        : directory.string();
}

// Binding stops at the first missing symbol: a half-bound Api would leave a
// null in place of a function and fail as a crash somewhere inside a backup
// rather than as a diagnosis here.
bool bindAll()
{
    bool bound = true;

#define VDDK_BIND_ENTRY_POINT(member, symbol) \
    bound = bound && bindEntryPoint(g_api.member, #symbol);

    VDDK_ENTRY_POINTS(VDDK_BIND_ENTRY_POINT)

#undef VDDK_BIND_ENTRY_POINT

    return bound;
}

} // namespace

bool Vddk::loadLibrary(const std::string& directory)
{
    if (g_loaded) {
        return true;
    }

    std::string reason;
    if (!Loader::open(directory, reason)) {
        std::cerr << "Error: failed to load '" << Loader::libraryName() << "'";
        if (!directory.empty()) {
            std::cerr << " from '" << directory << "'";
        }
        std::cerr << ": " << reason << "\n" << Loader::searchHint();
        return false;
    }

    if (!bindAll()) {
        Loader::close();
        g_api = Api{};
        return false;
    }

    rememberInstallRoot();
    g_loaded = true;
    return true;
}

const std::string& Vddk::installRoot()
{
    return g_installRoot;
}

const Vddk::Api& Vddk::api()
{
    return g_api;
}
