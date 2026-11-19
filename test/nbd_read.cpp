// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Test helper for nbd-selftest.sh: reads an NBD export with Nbd::DiskReader, prints the context, the geometry and the
// ranges, and writes what it read into a sparse image of the export's size.
//
//   abr-disk-nbd-read SERVER EXPORT|- BITMAP|- OUTPUT [--discard] [--rate BYTES_PER_SECOND]
//
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "disk_device.h"
#include "nbd_disk.h"

namespace {

constexpr uint32_t kChunk = 4 * 1024 * 1024;

int usage()
{
    std::cerr << "Usage: abr-disk-nbd-read SERVER EXPORT|- BITMAP|- OUTPUT [--discard] [--rate BYTES_PER_SECOND]\n";
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 5) {
        return usage();
    }

    Nbd::ReaderParams params;
    params.server = argv[1];
    if (std::string(argv[2]) != "-") {
        params.exportName = argv[2];
    }
    if (std::string(argv[3]) != "-") {
        params.bitmap = argv[3];
    }
    for (int i = 5; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--discard") {
            params.discard = true;
        } else if (option == "--rate" && i + 1 < argc) {
            params.bytesPerSecond = std::stoull(argv[++i]);
        } else {
            return usage();
        }
    }

    Nbd::DiskReader reader;
    std::vector<ByteRange> ranges;
    if (!reader.open(params) || !reader.dataRanges(ranges)) {
        return 1;
    }

    const Nbd::ExportProperties& properties = reader.properties();
    const DiskGeometry& geometry = reader.geometry();
    std::cout << "context " << (properties.context.empty() ? "-" : properties.context) << "\n"
              << "geometry " << geometry.sectors << " " << geometry.sectorSize << " " << geometry.phySectorSize << "\n";
    for (const ByteRange& range : ranges) {
        std::cout << "range " << range.offset << " " << range.length << "\n";
    }

    FileDiskWriter writer;
    if (!writer.open(argv[4]) || !writer.setImageSize(properties.size)) {
        return 1;
    }

    std::vector<uint8_t> buffer(kChunk);
    for (const ByteRange& range : ranges) {
        for (uint64_t done = 0; done < range.length;) {
            const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(range.length - done, kChunk));
            if (!reader.read(range.offset + done, count, buffer.data())
                || !writer.write(range.offset + done, count, buffer.data())) {
                return 1;
            }
            done += count;
        }
    }
    if (!writer.finish()) {
        return 1;
    }

    std::cout << "discarded " << reader.discardedBytes() << "\n";
    return 0;
}
