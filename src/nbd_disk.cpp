// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Disk source exported over NBD, read with libnbd
//
#include "nbd_disk.h"

#include <libnbd.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#include "disk_detail.h"

namespace {

// The sector size a backup records. NBD has no notion of one, and QEMU addresses its disks in 512-byte sectors.
constexpr uint32_t kSectorSize = 512;

// The largest single read, within what a server without extended headers accepts.
constexpr uint64_t kMaxRequest = 32ull * 1024 * 1024;

// How much one block status request asks about. It is a hint, and the server may answer for less.
constexpr uint64_t kStatusRequest = 1ull << 30;

bool nbdError(const char* what)
{
    const char* message = nbd_get_error();
    std::cerr << "Error: NBD " << what << " failed: " << (message ? message : "unknown error") << "\n";
    return false;
}

bool isUri(const std::string& server)
{
    return server.find("://") != std::string::npos;
}

// Appends a range, merged with the one before it when the two touch.
void addRange(std::vector<ByteRange>& ranges, uint64_t offset, uint64_t length)
{
    if (!ranges.empty() && ranges.back().offset + ranges.back().length == offset) {
        ranges.back().length += length;
    } else {
        ranges.push_back({offset, length});
    }
}

// One block status request's answer, collected by the callback below.
struct StatusWalk {
    const std::string* context = nullptr;
    // Keep dirty extents of a bitmap, or extents of base:allocation that are not known to read as zero.
    bool dirtyBitmap = false;
    uint64_t size = 0;
    // Where the reported extents end; the next request starts there.
    uint64_t end = 0;
    std::vector<ByteRange>* ranges = nullptr;
};

int collectExtents(void* userData, const char* metacontext, uint64_t offset, nbd_extent* entries, size_t count,
                   int* error)
{
    auto& walk = *static_cast<StatusWalk*>(userData);

    // A context other than the one asked for, or an answer after an earlier error, is not counted.
    if (*error != 0 || walk.context->compare(metacontext) != 0) {
        return 0;
    }

    uint64_t position = offset;
    for (size_t i = 0; i < count && position < walk.size && entries[i].length != 0; ++i) {
        const uint64_t length = std::min<uint64_t>(entries[i].length, walk.size - position);
        const bool keep = walk.dirtyBitmap ? (entries[i].flags & LIBNBD_STATE_DIRTY) != 0
                                           : (entries[i].flags & LIBNBD_STATE_ZERO) == 0;
        if (keep) {
            addRange(*walk.ranges, position, length);
        }
        position += length;
    }

    walk.end = std::max(walk.end, position);
    return 0;
}

} // namespace

class Nbd::DiskReader::Impl final : public IDiskReader
{
public:
    ~Impl() override
    {
        if (m_handle) {
            // A clean disconnect where the connection is up; on one that never connected this fails, harmlessly.
            nbd_shutdown(m_handle, 0);
            nbd_close(m_handle);
        }
    }

    bool open(const ReaderParams& params);

    const DiskGeometry& geometry() const override { return m_geometry; }
    bool dataRanges(std::vector<ByteRange>& ranges) override;
    bool read(uint64_t offset, uint32_t length, void* pBuf) override;

    ExportProperties m_properties;
    uint64_t m_discarded = 0;

private:
    bool connect(const ReaderParams& params);
    void discard(uint64_t offset, uint64_t length);
    void throttle(uint64_t bytes);

    struct nbd_handle* m_handle = nullptr;
    DiskGeometry m_geometry;
    std::string m_requested;
    uint64_t m_maxRequest = kMaxRequest;
    bool m_discard = false;

    // The rate limit and how far reads have got against it.
    uint64_t m_bytesPerSecond = 0;
    uint64_t m_throttled = 0;
    bool m_started = false;
    std::chrono::steady_clock::time_point m_start;
};

bool Nbd::DiskReader::Impl::connect(const ReaderParams& params)
{
    const bool uri = isUri(params.server);
    int result = 0;

    if (uri) {
        result = nbd_connect_uri(m_handle, params.server.c_str());
    } else if (nbd_set_export_name(m_handle, params.exportName.c_str()) == -1) {
        return nbdError("export name");
    } else {
        result = nbd_connect_unix(m_handle, params.server.c_str());
    }

    if (result == -1) {
        const char* message = nbd_get_error();
        std::cerr << "Error: cannot connect to NBD server '" << params.server << "'";
        if (!uri) {
            std::cerr << ", export '" << params.exportName << "'";
        }
        std::cerr << ": " << (message ? message : "unknown error") << "\n";
        return false;
    }
    return true;
}

bool Nbd::DiskReader::Impl::open(const ReaderParams& params)
{
    if (params.server.empty()) {
        std::cerr << "Error: no NBD server given\n";
        return false;
    }
    if (isUri(params.server) && !params.exportName.empty()) {
        std::cerr << "Error: the NBD URI '" << params.server << "' names its own export; an export name goes with "
                  << "a socket path\n";
        return false;
    }

    m_handle = nbd_create();
    if (!m_handle) {
        return nbdError("handle");
    }

    // One context per connection, so that a clean bitmap cannot break the connection (see the header).
    m_requested = params.bitmap.empty() ? std::string(LIBNBD_CONTEXT_BASE_ALLOCATION)
                                        : std::string(LIBNBD_CONTEXT_QEMU_DIRTY_BITMAP) + params.bitmap;
    if (nbd_add_meta_context(m_handle, m_requested.c_str()) == -1) {
        return nbdError("meta context request");
    }

    if (!connect(params)) {
        return false;
    }

    const int64_t size = nbd_get_size(m_handle);
    if (size < 0) {
        return nbdError("size query");
    }
    if (size % kSectorSize != 0) {
        std::cerr << "Error: the NBD export holds " << size << " bytes, which is not a whole number of "
                  << kSectorSize << "-byte sectors\n";
        return false;
    }
    m_properties.size = static_cast<uint64_t>(size);

    const int64_t preferred = nbd_get_block_size(m_handle, LIBNBD_SIZE_PREFERRED);
    const int64_t maximum = nbd_get_block_size(m_handle, LIBNBD_SIZE_MAXIMUM);
    const int64_t payload = nbd_get_block_size(m_handle, LIBNBD_SIZE_PAYLOAD);
    m_properties.preferredBlockSize = preferred > 0 ? static_cast<uint32_t>(preferred) : 0;
    m_properties.maximumBlockSize = maximum > 0 ? static_cast<uint32_t>(maximum) : 0;

    // Reads are split to what the server accepts in one request, kept a whole number of sectors.
    if (maximum > 0) {
        m_maxRequest = std::min<uint64_t>(m_maxRequest, static_cast<uint64_t>(maximum));
    }
    if (payload > 0) {
        m_maxRequest = std::min<uint64_t>(m_maxRequest, static_cast<uint64_t>(payload));
    }
    m_maxRequest = std::max<uint64_t>(kSectorSize, m_maxRequest - m_maxRequest % kSectorSize);

    // The physical sector size is recorded for information only.
    // The preferred block size stands for it when it is one a disk could have.
    m_geometry.sectorSize = kSectorSize;
    m_geometry.sectors = m_properties.size / kSectorSize;
    const bool diskLike = preferred >= 512 && preferred <= 4096 && (preferred & (preferred - 1)) == 0;
    m_geometry.phySectorSize = diskLike ? static_cast<uint32_t>(preferred) : kSectorSize;

    m_properties.readOnly = nbd_is_read_only(m_handle) == 1;
    m_properties.canTrim = nbd_can_trim(m_handle) == 1;

    const int offered = nbd_can_meta_context(m_handle, m_requested.c_str());
    if (offered == -1) {
        return nbdError("meta context query");
    }
    if (offered == 1) {
        m_properties.context = m_requested;
    } else if (!params.bitmap.empty()) {
        // An incremental read in full would be a full mislabelled as an incremental.
        std::cerr << "Error: the NBD export offers no dirty bitmap '" << params.bitmap << "'\n";
        return false;
    }

    m_discard = params.discard;
    if (m_discard && (m_properties.readOnly || !m_properties.canTrim)) {
        std::cerr << "Warning: the NBD export cannot discard, so ranges are kept after they are read\n";
        m_discard = false;
    }
    m_bytesPerSecond = params.bytesPerSecond;
    return true;
}

bool Nbd::DiskReader::Impl::dataRanges(std::vector<ByteRange>& ranges)
{
    ranges.clear();

    // Without allocation status the whole disk is data.
    if (m_properties.context.empty()) {
        if (m_properties.size != 0) {
            ranges.push_back({0, m_properties.size});
        }
        return true;
    }

    StatusWalk walk;
    walk.context = &m_properties.context;
    walk.dirtyBitmap = m_properties.context != LIBNBD_CONTEXT_BASE_ALLOCATION;
    walk.size = m_properties.size;
    walk.ranges = &ranges;

    uint64_t offset = 0;
    while (offset < m_properties.size) {
        walk.end = offset;
        nbd_extent64_callback callback = {collectExtents, &walk, nullptr};

        if (nbd_block_status_64(m_handle, std::min(m_properties.size - offset, kStatusRequest), offset, callback, 0)
            == -1) {
            return nbdError("block status");
        }

        // An answer that covers nothing would make the walk loop for ever.
        if (walk.end <= offset) {
            std::cerr << "Error: the NBD server reported no block status for '" << m_properties.context
                      << "' at offset " << offset << "\n";
            return false;
        }
        offset = walk.end;
    }
    return true;
}

bool Nbd::DiskReader::Impl::read(uint64_t offset, uint32_t length, void* pBuf)
{
    if (offset > m_properties.size || length > m_properties.size - offset) {
        std::cerr << "Error: a read of " << length << " bytes at offset " << offset << " is past the end of the "
                  << "NBD export (" << m_properties.size << " bytes)\n";
        return false;
    }

    if (m_bytesPerSecond != 0 && !m_started) {
        m_started = true;
        m_start = std::chrono::steady_clock::now();
    }

    auto* pBytes = static_cast<uint8_t*>(pBuf);
    for (uint64_t done = 0; done < length;) {
        const uint64_t count = std::min<uint64_t>(length - done, m_maxRequest);
        if (nbd_pread(m_handle, pBytes + done, count, offset + done, 0) == -1) {
            return nbdError("read");
        }
        done += count;
    }

    if (m_discard) {
        discard(offset, length);
    }
    throttle(length);
    return true;
}

// The data is in the caller's buffer by now. A discard that fails costs only the space it would have freed, so it
// is reported once and not tried again.
void Nbd::DiskReader::Impl::discard(uint64_t offset, uint64_t length)
{
    for (uint64_t done = 0; done < length;) {
        const uint64_t count = std::min(length - done, m_maxRequest);
        if (nbd_trim(m_handle, count, offset + done, 0) == -1) {
            const char* message = nbd_get_error();
            std::cerr << "Warning: NBD discard failed, so ranges are kept from here on: "
                      << (message ? message : "unknown error") << "\n";
            m_discard = false;
            return;
        }
        done += count;
        m_discarded += count;
    }
}

// Waits while the reads are ahead of the rate limit.
void Nbd::DiskReader::Impl::throttle(uint64_t bytes)
{
    if (m_bytesPerSecond == 0) {
        return;
    }
    m_throttled += bytes;

    const auto due = m_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(static_cast<double>(m_throttled) / m_bytesPerSecond));
    if (due > std::chrono::steady_clock::now()) {
        std::this_thread::sleep_until(due);
    }
}

Nbd::DiskReader::DiskReader() = default;
Nbd::DiskReader::~DiskReader() = default;

bool Nbd::DiskReader::open(const ReaderParams& params)
{
    auto pImpl = std::make_unique<Impl>();
    if (!pImpl->open(params)) {
        return false;
    }
    m_impl = std::move(pImpl);
    return true;
}

const Nbd::ExportProperties& Nbd::DiskReader::properties() const
{
    static const ExportProperties none;
    return m_impl ? m_impl->m_properties : none;
}

uint64_t Nbd::DiskReader::discardedBytes() const
{
    return m_impl ? m_impl->m_discarded : 0;
}

ABR_DISK_FORWARD_READER(Nbd::DiskReader)

std::unique_ptr<Nbd::DiskReader> Nbd::openReader(const ReaderParams& params)
{
    auto pReader = std::make_unique<DiskReader>();
    if (!pReader->open(params)) {
        return nullptr;
    }
    return pReader;
}
