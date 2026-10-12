// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Small Win32 helpers shared by the disk layers and the tools above them
//
#include "win_util.h"

#include <windows.h>

#include <shellapi.h>
#include <winioctl.h>

#include <cstdio>
#include <cwchar>

std::wstring Win::widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Win::narrow(const wchar_t* text, size_t length)
{
    if (length == 0) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string Win::narrow(const std::wstring& text)
{
    return narrow(text.data(), text.size());
}

namespace {

// FormatMessage in one language, or nothing when the system has no text for
// the code in that language.
std::wstring systemMessage(DWORD code, DWORD languageId)
{
    wchar_t* pBuffer = nullptr;

    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER,
        nullptr, code, languageId, reinterpret_cast<wchar_t*>(&pBuffer), 0, nullptr);

    std::wstring message(pBuffer ? pBuffer : L"", length);
    LocalFree(pBuffer);
    return message;
}

} // namespace

std::string Win::errorText(DWORD code)
{
    std::wstring message = systemMessage(code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    if (message.empty()) {
        message = systemMessage(code, 0);
    }

    // System messages end in a full stop and a line break, which would land in
    // the middle of the line this is spliced into.
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r'
                                || message.back() == L' ' || message.back() == L'.')) {
        message.pop_back();
    }

    // The low Win32 codes are the ones people look up by number; the ones
    // virtdisk.dll returns carry a facility and read as hex everywhere else.
    char number[32];
    if (code < 0x10000) {
        std::snprintf(number, sizeof(number), "error %lu", static_cast<unsigned long>(code));
    } else {
        std::snprintf(number, sizeof(number), "error 0x%08lX", static_cast<unsigned long>(code));
    }

    if (message.empty()) {
        return number;
    }
    return narrow(message) + " (" + number + ")";
}

std::vector<std::string> Win::commandLineArguments()
{
    std::vector<std::string> arguments;

    int count = 0;
    wchar_t** pArguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!pArguments) {
        return arguments;
    }

    for (int i = 0; i < count; ++i) {
        arguments.push_back(narrow(pArguments[i], wcslen(pArguments[i])));
    }

    LocalFree(pArguments);
    return arguments;
}

bool Win::isElevated()
{
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) {
        return false;
    }

    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

bool Win::enablePrivilege(const wchar_t* name)
{
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.put())) {
        return false;
    }

    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) {
        return false;
    }

    // AdjustTokenPrivileges reports success even for a privilege it could not
    // enable, and says so only through the last error.
    if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr)) {
        return false;
    }
    return GetLastError() == ERROR_SUCCESS;
}

bool Win::sectorSizes(HANDLE handle, uint32_t& logical, uint32_t& physical)
{
    STORAGE_PROPERTY_QUERY request = {};
    request.PropertyId = StorageAccessAlignmentProperty;
    request.QueryType = PropertyStandardQuery;

    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment = {};
    DWORD returned = 0;
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &request, sizeof(request),
                        &alignment, sizeof(alignment), &returned, nullptr)
        && alignment.Size >= sizeof(alignment) && alignment.BytesPerLogicalSector > 0) {
        logical = alignment.BytesPerLogicalSector;

        // A device that reports a logical size but no physical one is described
        // by the one it did report.
        physical = alignment.BytesPerPhysicalSector > 0 ? alignment.BytesPerPhysicalSector
                                                        : alignment.BytesPerLogicalSector;
        return true;
    }

    // Not every device answers the alignment query -- a volume usually does
    // not -- and the classic geometry has the one sector size, which then
    // stands for both.
    DISK_GEOMETRY classic = {};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &classic,
                        sizeof(classic), &returned, nullptr)
        && classic.BytesPerSector > 0) {
        logical = classic.BytesPerSector;
        physical = classic.BytesPerSector;
        return true;
    }
    return false;
}

namespace {

// The offset rides in the OVERLAPPED even on a synchronous handle, which makes
// every transfer independent of the file pointer and of whatever moved it last.
OVERLAPPED overlappedAt(uint64_t offset)
{
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return overlapped;
}

} // namespace

DWORD Win::readAt(HANDLE handle, uint64_t offset, void* pBuf, uint32_t length)
{
    auto* pBytes = static_cast<uint8_t*>(pBuf);
    uint32_t done = 0;

    while (done < length) {
        OVERLAPPED overlapped = overlappedAt(offset + done);
        DWORD count = 0;

        if (!ReadFile(handle, pBytes + done, length - done, &count, &overlapped)) {
            return GetLastError();
        }
        if (count == 0) {
            return ERROR_HANDLE_EOF;
        }
        done += count;
    }
    return ERROR_SUCCESS;
}

DWORD Win::writeAt(HANDLE handle, uint64_t offset, const void* pBuf, uint32_t length)
{
    const auto* pBytes = static_cast<const uint8_t*>(pBuf);
    uint32_t done = 0;

    while (done < length) {
        OVERLAPPED overlapped = overlappedAt(offset + done);
        DWORD count = 0;

        if (!WriteFile(handle, pBytes + done, length - done, &count, &overlapped)) {
            return GetLastError();
        }
        if (count == 0) {
            return ERROR_HANDLE_EOF;
        }
        done += count;
    }
    return ERROR_SUCCESS;
}
