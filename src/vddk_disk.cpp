// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VMware VDDK disk source/target
//
#include "vddk_disk.h"

#include <algorithm>
#include <iostream>

#include "disk_detail.h"
#include "vddk_api.h"

namespace {

constexpr uint32_t kVersionMajor = 6;
constexpr uint32_t kVersionMinor = 8;

// The VDDK entry points, bound by Vddk::loadLibrary. Nothing in this file may
// call through them before Session::connect has done that.
const Vddk::Api& vddk()
{
    return Vddk::api();
}

void reportError(const std::string& what, VixError vixError)
{
    char* msg = vddk().getErrorText(vixError, nullptr);
    std::cerr << "Error: " << what << ": " << (msg ? msg : "unknown VDDK error") << "\n";
    vddk().freeErrorText(msg);
}

// Every VDDK call below follows the same "check, complain, bail" shape; folding
// it here keeps the callers readable and makes the message the only variable.
bool vixOk(const std::string& what, VixError vixError)
{
    if (VIX_FAILED(vixError)) {
        reportError(what, vixError);
        return false;
    }
    return true;
}

// VixDiskLibConnectParams stores borrowed char* pointers; an empty option must
// stay NULL rather than point at an empty string.
char* optionalStr(const std::string& value)
{
    return value.empty() ? nullptr : const_cast<char*>(value.c_str());
}

// The lists VDDK hands back have to be released with their own free function,
// and the query loops have failure paths in the middle. Owning them makes those
// paths leak-free without a goto or a second free at every exit.
struct BlockListDeleter {
    void operator()(VixDiskLibBlockList* list) const { vddk().freeBlockList(list); }
};
using BlockListPtr = std::unique_ptr<VixDiskLibBlockList, BlockListDeleter>;

struct ChangeInfoDeleter {
    void operator()(VixDiskLibDiskChangeInfo* info) const { vddk().freeChangedAreas(info); }
};
using ChangeInfoPtr = std::unique_ptr<VixDiskLibDiskChangeInfo, ChangeInfoDeleter>;

// Out of range chunk sizes make the allocated-block query fail with an
// opaque error, and zero would divide by zero below, so reject them up front.
bool validQueryChunk(uint64_t sectors)
{
    if (sectors < VIXDISKLIB_MIN_CHUNK_SIZE || sectors > VIXDISKLIB_MAX_CHUNK_SIZE) {
        std::cerr << "Error: query chunk of " << sectors << " sectors is outside the range VDDK "
                  << "accepts [" << VIXDISKLIB_MIN_CHUNK_SIZE << ", " << VIXDISKLIB_MAX_CHUNK_SIZE
                  << "]\n";
        return false;
    }
    return true;
}

class VddkDisk : public IDiskReader, public IDiskWriter
{
public:
    VddkDisk(const std::string& changeId, uint64_t queryChunkSize)
        : m_changeId(changeId)
        , m_queryChunkSize(queryChunkSize)
    {}

    ~VddkDisk() override
    {
        if (m_info) {
            vddk().freeInfo(m_info);
        }
        if (m_handle) {
            vddk().close(m_handle);
        }
    }

    bool open(VixDiskLibConnection connection, const std::string& diskPath, bool readOnly)
    {
        VixDiskLibOpenParams openParams{const_cast<char*>(diskPath.c_str()),
                                        readOnly ? VIXDISKLIB_FLAG_OPEN_READ_ONLY : 0u,
                                        optionalStr(m_changeId)};

        if (!vixOk("failed to open disk '" + diskPath + "'",
                   vddk().openEx(connection, &openParams, &m_handle))) {
            m_handle = nullptr;
            return false;
        }

        if (!vixOk("failed to query info of disk '" + diskPath + "'",
                   vddk().getInfo(m_handle, &m_info))) {
            m_info = nullptr;
            return false;
        }

        m_geometry.sectorSize = m_info->logicalSectorSize;
        m_geometry.phySectorSize = m_info->physicalSectorSize;
        m_geometry.sectors = m_info->capacity;

        // Everything below converts between bytes and sectors by dividing, so a
        // disk that reports no sector size has to be rejected here.
        if (m_geometry.sectorSize == 0) {
            std::cerr << "Error: disk '" << diskPath << "' reports a zero logical sector size\n";
            return false;
        }

        std::cerr << "Disk '" << diskPath << "' opened using transport mode '"
                  << vddk().getTransportMode(m_handle) << "'\n";
        return true;
    }

    const DiskGeometry& geometry() const override { return m_geometry; }

    bool dataRanges(std::vector<ByteRange>& ranges) override
    {
        ranges.clear();
        return m_changeId.empty() ? queryAllocatedBlocks(ranges) : queryChangedAreas(ranges);
    }

    bool read(uint64_t offset, uint32_t length, void* pBuf) override
    {
        return checkAlignment("read", offset, length)
            && vixOk("failed to read disk data",
                     vddk().read(m_handle, offset / m_geometry.sectorSize,
                                 length / m_geometry.sectorSize, static_cast<uint8*>(pBuf)));
    }

    bool write(uint64_t offset, uint32_t length, const void* pBuf) override
    {
        return checkAlignment("write", offset, length)
            && vixOk("failed to write disk data",
                     vddk().write(m_handle, offset / m_geometry.sectorSize,
                                  length / m_geometry.sectorSize,
                                  static_cast<const uint8*>(pBuf)));
    }

    // The disk already has the geometry it was created with; a restore cannot
    // change it, and whether the two agree is the caller's to check.
    bool setImageSize(uint64_t) override { return true; }

    bool finish() override { return true; }

private:
    bool checkAlignment(const char* op, uint64_t offset, uint32_t length) const
    {
        if (offset % m_geometry.sectorSize != 0 || length % m_geometry.sectorSize != 0) {
            std::cerr << "Error: unaligned disk " << op << " of " << length
                      << " bytes at offset " << offset << " (sector size "
                      << m_geometry.sectorSize << ")\n";
            return false;
        }
        return true;
    }

    void appendRange(std::vector<ByteRange>& ranges, const VixDiskLibBlock& block) const
    {
        ranges.push_back({block.offset * m_geometry.sectorSize,
                          block.length * m_geometry.sectorSize});
    }

    void appendBlocks(std::vector<ByteRange>& ranges, const VixDiskLibBlockList& list) const
    {
        for (uint32 i = 0; i < list.numBlocks; i++) {
            appendRange(ranges, list.blocks[i]);
        }
    }

    bool queryAllocatedBlocks(std::vector<ByteRange>& ranges)
    {
        if (!validQueryChunk(m_queryChunkSize)) {
            return false;
        }

        const uint64_t capacity = m_geometry.sectors;
        // VDDK reports whole chunks only, so the walk covers the disk up to the
        // last chunk boundary and the remainder is handled after the loop.
        const uint64_t queryable = capacity - capacity % m_queryChunkSize;
        const uint64_t maxPerQuery = m_queryChunkSize * VIXDISKLIB_MAX_CHUNK_NUMBER;
        uint64_t offset = 0;

        while (offset < queryable) {
            const uint64_t sectors = std::min(queryable - offset, maxPerQuery);

            VixDiskLibBlockList* rawList = nullptr;
            VixError vixError = vddk().queryAllocatedBlocks(
                m_handle, offset, sectors, m_queryChunkSize, &rawList);
            BlockListPtr blockList(rawList);
            if (!vixOk("failed to query allocated blocks", vixError)) {
                return false;
            }

            if (blockList) {
                appendBlocks(ranges, *blockList);
            }
            offset += sectors;
        }

        // The tail below one chunk cannot be queried, so back it up unconditionally.
        if (offset < capacity) {
            appendRange(ranges, VixDiskLibBlock{offset, capacity - offset});
        }
        return true;
    }

    bool queryChangedAreas(std::vector<ByteRange>& ranges)
    {
        VixDiskLibQueryChangedAreaParams queryParams{0};

        while (queryParams.startOffset < m_geometry.sectors) {
            VixDiskLibDiskChangeInfo* rawInfo = nullptr;
            VixError vixError = vddk().queryChangedAreas(m_handle, &queryParams, &rawInfo);
            ChangeInfoPtr changeList(rawInfo);
            if (!vixOk("failed to query changed areas", vixError)) {
                return false;
            }

            if (changeList->areas) {
                appendBlocks(ranges, *changeList->areas);
            }

            // A reply that does not move past the offset we asked about would
            // spin here forever, so treat it as the end of the disk.
            const uint64_t next = changeList->startOffset + changeList->length;
            if (next <= queryParams.startOffset) {
                break;
            }
            queryParams.startOffset = next;
        }
        return true;
    }

    VixDiskLibHandle m_handle = nullptr;
    VixDiskLibInfo* m_info = nullptr;
    DiskGeometry m_geometry;
    std::string m_changeId;
    uint64_t m_queryChunkSize;
};

// Points the VDDK struct at the strings the session owns. Only the VMX spec is
// supported: a disk is addressed by its VM, not by datastore or FCD.
void fillConnectParams(VixDiskLibConnectParams& out, const Vddk::ConnectParams& params)
{
    out.serverName = optionalStr(params.host);
    out.credType = VIXDISKLIB_CRED_UID;
    out.creds.uid.userName = optionalStr(params.user);
    out.creds.uid.password = optionalStr(params.password);
    out.thumbPrint = optionalStr(params.thumbPrint);
    out.port = params.port;
    out.nfcHostPort = params.nfcHostPort;
    out.specType = VIXDISKLIB_SPEC_VMX;
    out.vmxSpec = optionalStr(params.vmxSpec);
}

} // namespace

Vddk::Session::~Session()
{
    auto* connectParams = static_cast<VixDiskLibConnectParams*>(m_connectParams);

    if (m_connection) {
        vddk().disconnect(static_cast<VixDiskLibConnection>(m_connection));
    }
    if (m_accessPrepared) {
        vddk().endAccess(connectParams, m_params.identity.c_str());
    }
    if (connectParams) {
        vddk().freeConnectParams(connectParams);
    }
    if (m_initialized) {
        vddk().exit();
    }
}

bool Vddk::Session::connect(const ConnectParams& params, bool readOnly)
{
    if (m_initialized) {
        std::cerr << "Error: VDDK session is already connected\n";
        return false;
    }
    // A remote disk is addressed as "this VMDK of that VM", so the host alone is
    // not enough to locate it.
    if (!params.host.empty() && params.vmxSpec.empty()) {
        std::cerr << "Error: a VM spec is required to open a disk on host '" << params.host << "'\n";
        return false;
    }

    m_params = params;

    // Nothing above this line may touch the VDDK: the entry points are bound
    // here, from whatever installation the library search path leads to.
    if (!loadLibrary(m_params.libraryDir)) {
        return false;
    }

    // Init is told where that installation turned out to be so that it looks
    // for the transport plugins in the same bundle the library came from.
    if (!vixOk("failed to initialize VDDK",
               vddk().init(kVersionMajor, kVersionMinor, nullptr, nullptr, nullptr,
                           optionalStr(installRoot())))) {
        return false;
    }
    m_initialized = true;

    VixDiskLibConnectParams* connectParams = vddk().allocateConnectParams();
    if (!connectParams) {
        std::cerr << "Error: failed to allocate VDDK connection parameters\n";
        return false;
    }
    m_connectParams = connectParams;

    // Without a host the VMDK is opened straight off the local filesystem, which
    // needs neither credentials nor the VM to be put into backup mode.
    if (!m_params.host.empty()) {
        fillConnectParams(*connectParams, m_params);

        VixError vixError = vddk().prepareForAccess(connectParams, m_params.identity.c_str());

        // VIX_E_OPERATION_DISABLED here means the VM still carries a disable put
        // in place under our own identity, which can only be a leftover: a run
        // that died between PrepareForAccess and EndAccess never got to release it.
        // Releasing it is exactly what EndAccess is for, so clear it and try
        // once more instead of leaving the VM unbackupable until someone runs
        // another tool by hand.
        if (VIX_ERROR_CODE(vixError) == VIX_E_OPERATION_DISABLED) {
            std::cerr << "Warning: clearing an access lock left behind by an earlier run\n";
            vddk().endAccess(connectParams, m_params.identity.c_str());
            vixError = vddk().prepareForAccess(connectParams, m_params.identity.c_str());
        }

        // A standalone ESXi host has no vCenter to hold the relocation lock
        // PrepareForAccess takes, and answers VIX_E_NOT_SUPPORTED_ON_REMOTE_OBJECT.
        // Nothing can move the VM there behind our back anyway, so there is
        // nothing to prepare and, with m_accessPrepared left false, nothing for
        // EndAccess to release either.
        if (VIX_ERROR_CODE(vixError) != VIX_E_NOT_SUPPORTED_ON_REMOTE_OBJECT) {
            if (!vixOk("failed to prepare the virtual machine for access", vixError)) {
                return false;
            }
            m_accessPrepared = true;
        }
    }

    VixDiskLibConnection connection = nullptr;
    if (!vixOk("failed to connect",
               vddk().connectEx(connectParams, readOnly, optionalStr(m_params.snapshotRef),
                                optionalStr(m_params.transportModes), &connection))) {
        return false;
    }
    m_connection = connection;

    return true;
}

Vddk::DiskReader::DiskReader() = default;
Vddk::DiskReader::~DiskReader() = default;

bool Vddk::DiskReader::open(Session& session, const std::string& diskPath,
                            const std::string& changeId, uint64_t queryChunkSize)
{
    m_impl.reset();

    if (!session.m_connection) {
        std::cerr << "Error: the VDDK session is not connected\n";
        return false;
    }

    auto disk = std::make_unique<VddkDisk>(changeId, queryChunkSize);
    if (!disk->open(static_cast<VixDiskLibConnection>(session.m_connection), diskPath, true)) {
        return false;
    }
    m_impl = std::move(disk);
    return true;
}

ABR_DISK_FORWARD_READER(Vddk::DiskReader)

Vddk::DiskWriter::DiskWriter() = default;
Vddk::DiskWriter::~DiskWriter() = default;

bool Vddk::DiskWriter::open(Session& session, const std::string& diskPath)
{
    m_impl.reset();

    if (!session.m_connection) {
        std::cerr << "Error: the VDDK session is not connected\n";
        return false;
    }

    auto disk = std::make_unique<VddkDisk>("", 0);
    if (!disk->open(static_cast<VixDiskLibConnection>(session.m_connection), diskPath, false)) {
        return false;
    }
    m_impl = std::move(disk);
    return true;
}

ABR_DISK_FORWARD_WRITER(Vddk::DiskWriter)

std::unique_ptr<IDiskReader> Vddk::Session::openReader(const std::string& diskPath,
                                                       const std::string& changeId,
                                                       uint64_t queryChunkSize)
{
    auto reader = std::make_unique<DiskReader>();
    if (!reader->open(*this, diskPath, changeId, queryChunkSize)) {
        return nullptr;
    }
    return reader;
}

std::unique_ptr<IDiskWriter> Vddk::Session::openWriter(const std::string& diskPath)
{
    auto writer = std::make_unique<DiskWriter>();
    if (!writer->open(*this, diskPath)) {
        return nullptr;
    }
    return writer;
}
