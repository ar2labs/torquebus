// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "plugins/driver-j2534/J2534Registry.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace torquebus::j2534 {

#ifdef _WIN32

namespace {

std::string wideToUtf8(const std::wstring_view wstr)
{
    if (wstr.empty()) {
        return {};
    }
    const int sizeNeeded = WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(sizeNeeded), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wstr.data(),
                        static_cast<int>(wstr.size()),
                        result.data(),
                        sizeNeeded,
                        nullptr,
                        nullptr);
    return result;
}

std::wstring readStringValue(HKEY key, const wchar_t* valueName)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS
        || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        return {};
    }

    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes)
        != ERROR_SUCCESS) {
        return {};
    }

    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }

    if (type == REG_EXPAND_SZ) {
        DWORD expandedLen = ExpandEnvironmentStringsW(buffer.c_str(), nullptr, 0);
        if (expandedLen > 0) {
            std::wstring expanded(expandedLen, L'\0');
            ExpandEnvironmentStringsW(buffer.c_str(), expanded.data(), expandedLen);
            while (!expanded.empty() && expanded.back() == L'\0') {
                expanded.pop_back();
            }
            return expanded;
        }
    }

    return buffer;
}

DWORD readDwordValue(HKEY key, const wchar_t* valueName, DWORD defaultValue = 0)
{
    DWORD type = 0;
    DWORD data = 0;
    DWORD bytes = sizeof(data);
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &bytes)
            == ERROR_SUCCESS
        && type == REG_DWORD) {
        return data;
    }
    return defaultValue;
}

} // namespace

std::vector<J2534DeviceInfo> enumerateJ2534Devices()
{
    std::vector<J2534DeviceInfo> devices;

    // SAE J2534-1 root registry path
    constexpr const wchar_t* kRegistryPath = L"SOFTWARE\\PassThruSupport.04.04";

    HKEY rootKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_READ | KEY_WOW64_64KEY, &rootKey)
        != ERROR_SUCCESS) {
        return devices;
    }

    DWORD subkeyCount = 0;
    DWORD maxSubkeyLen = 0;
    if (RegQueryInfoKeyW(rootKey,
                         nullptr,
                         nullptr,
                         nullptr,
                         &subkeyCount,
                         &maxSubkeyLen,
                         nullptr,
                         nullptr,
                         nullptr,
                         nullptr,
                         nullptr,
                         nullptr)
        != ERROR_SUCCESS) {
        RegCloseKey(rootKey);
        return devices;
    }

    std::wstring subkeyName(maxSubkeyLen + 1, L'\0');

    for (DWORD i = 0; i < subkeyCount; ++i) {
        DWORD nameLen = maxSubkeyLen + 1;
        if (RegEnumKeyExW(
                rootKey, i, subkeyName.data(), &nameLen, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS) {
            continue;
        }
        subkeyName.resize(nameLen);

        HKEY deviceKey = nullptr;
        if (RegOpenKeyExW(rootKey, subkeyName.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &deviceKey)
            != ERROR_SUCCESS) {
            subkeyName.resize(maxSubkeyLen + 1);
            continue;
        }

        const std::wstring name = readStringValue(deviceKey, L"Name");
        const std::wstring vendor = readStringValue(deviceKey, L"Vendor");
        const std::wstring functionLib = readStringValue(deviceKey, L"FunctionLibrary");
        const DWORD can = readDwordValue(deviceKey, L"CAN", 1);
        const DWORD canPs = readDwordValue(deviceKey, L"CAN_PS", 0);

        RegCloseKey(deviceKey);
        subkeyName.resize(maxSubkeyLen + 1);

        if (functionLib.empty()) {
            continue;
        }

        J2534DeviceInfo info;
        info.id = std::to_string(devices.size());
        info.name = !name.empty() ? wideToUtf8(name) : wideToUtf8(subkeyName);
        info.vendor = wideToUtf8(vendor);
        info.libraryPath = wideToUtf8(functionLib);
        info.canSupported = (can != 0 || canPs != 0);

        devices.push_back(std::move(info));
    }

    RegCloseKey(rootKey);
    return devices;
}

#else

std::vector<J2534DeviceInfo> enumerateJ2534Devices()
{
    return {};
}

#endif

} // namespace torquebus::j2534
