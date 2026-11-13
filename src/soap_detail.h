// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// What the users of vsphere-soap-lib in this library share
//
#pragma once

namespace Soap::Detail
{
    // Sends vsphere-soap-lib's own log lines to std::cerr behind "Warning: " or "Note: ", never "Error: ".
    // The library's default sink writes its errors as "Error: " lines, and the first of those is what a tool reports
    // as the failure, while the line that says which operation failed is the caller's own.
    // Safe to call any number of times.
    void routeLibraryLog();
}
