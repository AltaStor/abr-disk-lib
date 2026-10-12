// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Self-test of what can be tested without a hypervisor: the file backend,
// through the exported classes, and what an unopened disk answers with.
//
// The virtual disk backends need the real thing behind them -- an elevated
// process on a Hyper-V host, a VDDK and something for it to open -- and are
// tested from the tools, end to end. This only reports which of them the build
// carries.
//
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "disk_device.h"
#include "vddk_disk.h"
#include "vhdx_disk.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (!condition) {
        std::cout << "FAILED (line " << line << "): " << what << "\n";
        ++g_failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

std::vector<uint8_t> pattern(size_t size, uint8_t seed)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(seed + i * 31);
    }
    return data;
}

void testUnopened()
{
    FileDiskReader reader;
    FileDiskWriter writer;
    std::vector<ByteRange> ranges;
    uint8_t byte = 0;

    CHECK(reader.geometry().sectors == 0);
    CHECK(!reader.dataRanges(ranges));
    CHECK(!reader.read(0, 1, &byte));
    CHECK(writer.geometry().sectors == 0);
    CHECK(!writer.write(0, 1, &byte));
    CHECK(!writer.finish());
}

void testMissing(const std::string& directory)
{
    FileDiskReader reader;
    CHECK(!reader.open(directory + "/no-such-file.img"));
    CHECK(openFileReader(directory + "/no-such-file.img") == nullptr);

    // An incremental is applied onto a restored full, so there has to be one.
    FileDiskWriter writer;
    CHECK(!writer.open(directory + "/no-such-file.img", true));
}

void testRoundTrip(const std::string& directory)
{
    const std::string path = directory + "/disk.img";
    const uint64_t imageSize = 1024 * 1024;
    const std::vector<uint8_t> head = pattern(4096, 1);
    const std::vector<uint8_t> middle = pattern(8192, 77);

    {
        FileDiskWriter writer;
        CHECK(writer.open(path));

        // No fixed layout of its own, so nothing for a backup to be held against.
        CHECK(writer.geometry().sectors == 0);
        CHECK(writer.setImageSize(imageSize));
        CHECK(writer.write(0, static_cast<uint32_t>(head.size()), head.data()));
        CHECK(writer.write(65536, static_cast<uint32_t>(middle.size()), middle.data()));
        CHECK(writer.finish());
    }

    // Sized to the image, not to the last range written.
    CHECK(std::filesystem::file_size(std::filesystem::path(path)) == imageSize);

    {
        FileDiskReader reader;
        CHECK(reader.open(path));
        CHECK(reader.geometry().sectorSize == 512);
        CHECK(reader.geometry().phySectorSize == 512);
        CHECK(reader.geometry().sectors == imageSize / 512);

        std::vector<ByteRange> ranges;
        CHECK(reader.dataRanges(ranges));
        CHECK(ranges.size() == 1 && ranges[0].offset == 0 && ranges[0].length == imageSize);

        std::vector<uint8_t> buffer(middle.size());
        CHECK(reader.read(0, static_cast<uint32_t>(head.size()), buffer.data()));
        CHECK(std::memcmp(buffer.data(), head.data(), head.size()) == 0);
        CHECK(reader.read(65536, static_cast<uint32_t>(middle.size()), buffer.data()));
        CHECK(buffer == middle);

        // The gap between the two ranges, and the tail, read back as zeros.
        CHECK(reader.read(32768, 4096, buffer.data()));
        CHECK(std::all_of(buffer.begin(), buffer.begin() + 4096, [](uint8_t b) { return b == 0; }));
        CHECK(reader.read(imageSize - 4096, 4096, buffer.data()));
        CHECK(std::all_of(buffer.begin(), buffer.begin() + 4096, [](uint8_t b) { return b == 0; }));

        // Past the end is a failure, not a short read.
        CHECK(!reader.read(imageSize, 512, buffer.data()));
    }

    // An incremental onto the same target: what it does not write stays.
    const std::vector<uint8_t> patch = pattern(4096, 200);
    {
        auto writer = openFileWriter(path, true);
        CHECK(writer != nullptr);
        if (writer) {
            CHECK(writer->setImageSize(imageSize));
            CHECK(writer->write(65536, static_cast<uint32_t>(patch.size()), patch.data()));
            CHECK(writer->finish());
        }
    }
    {
        auto reader = openFileReader(path);
        CHECK(reader != nullptr);
        if (reader) {
            std::vector<uint8_t> buffer(head.size());
            CHECK(reader->read(0, static_cast<uint32_t>(head.size()), buffer.data()));
            CHECK(buffer == head);
            CHECK(reader->read(65536, static_cast<uint32_t>(patch.size()), buffer.data()));
            CHECK(buffer == patch);
            CHECK(reader->read(65536 + 4096, 4096, buffer.data()));
            CHECK(std::memcmp(buffer.data(), middle.data() + 4096, 4096) == 0);
        }
    }

    // A full restore onto the same target: what was there before goes.
    {
        FileDiskWriter writer;
        CHECK(writer.open(path, false));
        CHECK(writer.setImageSize(8192));
        CHECK(writer.write(4096, 4096, head.data()));
        CHECK(writer.finish());
    }
    {
        FileDiskReader reader;
        CHECK(reader.open(path));
        CHECK(reader.geometry().sectors == 16);

        std::vector<uint8_t> buffer(4096);
        CHECK(reader.read(0, 4096, buffer.data()));
        CHECK(std::all_of(buffer.begin(), buffer.end(), [](uint8_t b) { return b == 0; }));
    }

    // A regular file has no sector size for the host to report.
    uint32_t logical = 7;
    uint32_t physical = 9;
    CHECK(!Disk::deviceSectorSize(path, logical, physical));
    CHECK(logical == 7 && physical == 9);
}

void testBackends()
{
#ifdef ABR_DISK_HAVE_VHDX
    CHECK(Disk::hasVhdx());
    CHECK(Vhdx::isRemotePath("\\\\server\\share\\disk.vhdx"));
    CHECK(Vhdx::isRemotePath("\\\\?\\UNC\\server\\share\\disk.vhdx"));
    CHECK(Vhdx::isRemotePath("//server/share/disk.vhdx"));
    CHECK(!Vhdx::isRemotePath("C:\\VMs\\disk.vhdx"));
    CHECK(!Vhdx::isRemotePath("\\\\.\\PhysicalDrive1"));
    CHECK(!Vhdx::isRemotePath("\\\\?\\C:\\VMs\\disk.vhdx"));
    CHECK(!Vhdx::isRemotePath("\\\\server"));
#else
    CHECK(!Disk::hasVhdx());
#endif

#ifdef ABR_DISK_HAVE_VDDK
    CHECK(Disk::hasVddk());

    // No session, no disk -- and no VDDK is touched finding that out.
    Vddk::Session session;
    Vddk::DiskReader reader;
    CHECK(!reader.open(session, "[datastore] vm/vm.vmdk"));
#else
    CHECK(!Disk::hasVddk());
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: abr-disk-selftest SCRATCH_DIR\n";
        return 2;
    }

    const std::string directory = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(directory), ec);

    std::cout << "Backends: file/device" << (Disk::hasVhdx() ? ", Hyper-V VHD/VHDX" : "")
              << (Disk::hasVddk() ? ", VMware VDDK" : "") << "\n";

    // The failures these provoke are reported on std::cerr, as they would be
    // to a tool; the verdict is on std::cout.
    testUnopened();
    testMissing(directory);
    testRoundTrip(directory);
    testBackends();

    std::filesystem::remove_all(std::filesystem::path(directory), ec);

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All checks passed\n";
    return 0;
}
