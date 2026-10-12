// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Loading the VDDK on Windows
//
#include "vddk_loader.h"

#include <windows.h>

#include "win_util.h"

namespace {

constexpr const char* kLibraryName = "vixDiskLib.dll";

HMODULE g_module = nullptr;

} // namespace

const char* Vddk::Loader::libraryName()
{
    return kLibraryName;
}

bool Vddk::Loader::open(const std::string& directory, std::string& reason)
{
    if (directory.empty()) {
        // The standard search: beside the executable first, then PATH, which
        // is where a VDDK's bin directory is put for the programs that use it.
        g_module = LoadLibraryW(Win::widen(kLibraryName).c_str());
    } else {
        // The altered search path looks for the DLLs vixDiskLib.dll itself
        // imports beside it, in the bundle, rather than beside the executable
        // -- they are the bundle's own builds and it expects no others.
        const std::string path = directory + "\\" + kLibraryName;
        g_module = LoadLibraryExW(Win::widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    if (!g_module) {
        reason = Win::errorText(GetLastError());
        return false;
    }
    return true;
}

// GetProcAddress hands back a FARPROC, which is no more the entry point's type
// than a void* is; the caller casts it to the type vixDiskLib.h declares.
void* Vddk::Loader::symbol(const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(g_module, name));
}

std::string Vddk::Loader::origin()
{
    std::wstring path(MAX_PATH, L'\0');

    for (;;) {
        const DWORD length = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        // Truncated: the path is longer than the buffer.
        path.resize(path.size() * 2);
    }

    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::string() : Win::narrow(path.substr(0, slash));
}

const char* Vddk::Loader::searchHint()
{
    return "No VDDK installation was found: put its bin directory on PATH, or name it "
           "in Vddk::ConnectParams::libraryDir.\n";
}

void Vddk::Loader::close()
{
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}
