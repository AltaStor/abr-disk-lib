// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VMware disk source/target over the vSphere Web Services API, through vsphere-soap-lib
//
#include "soap_disk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <vsphere-soap/Log.hpp>
#include <vsphere-soap/VSphereDatastoreBackend.hpp>
#include <vsphere-soap/VSphereDiskImport.hpp>
#include <vsphere-soap/VSphereFolderPutBackend.hpp>
#include <vsphere-soap/VSphereFolderPutImport.hpp>
#include <vsphere-soap/VSphereImportSession.hpp>
#include <vsphere-soap/VSpherePlacement.hpp>
#include <vsphere-soap/VmwareSoapDisk.hpp>
#include <vsphere-soap/vim/HttpTransportAdapter.hpp>
#include <vsphere-soap/vim/SoapClient.hpp>
#include <vsphere-soap/vim/VimInventory.hpp>
#include <vsphere-soap/vim/VimSession.hpp>

#include "disk_detail.h"

namespace {

constexpr uint32_t kSectorSize = 512;

bool failed(const std::string& what, const VSphereSoap::Error& error)
{
    std::cerr << "Error: " << what << ": " << error.message() << "\n";
    return false;
}

// The library's own messages go to std::cerr like everything else here, but never as "Error: ": the line this
// backend writes when an operation fails is the one a tool reports.
void routeLibraryLog()
{
    static std::once_flag once;
    std::call_once(once, [] {
        using VSphereSoap::Log::Level;
        VSphereSoap::Log::setSink([](Level level, std::string_view channel, std::string_view message) {
            static std::mutex mutex; // the library logs from its upload threads too
            const std::lock_guard lock(mutex);
            std::cerr << (level >= Level::Warn ? "Warning: [" : "Note: [") << channel << "] "
                      << message << "\n";
        });
    });
}

VSphereSoap::ConnectParams toLibrary(const Soap::ConnectParams& params)
{
    VSphereSoap::ConnectParams out;
    out.host = params.host;
    out.port = static_cast<std::uint16_t>(params.port == 0 ? 443 : params.port);
    out.username = params.user;
    out.password = params.password;
    out.verifyTlsCertificate = true; // a thumbprint, when given, replaces the chain check
    out.pinnedThumbprint = params.thumbPrint;
    out.cbtSnapshotName = params.identity;
    out.snapshotMoref = params.snapshot;
    out.quiesce = params.quiesce;
    return out;
}

// "moref=vm-12" or "vm-12" -> "vm-12"; the first is how the VDDK's VM spec spells it.
std::string vmMoref(const std::string& vm)
{
    constexpr std::string_view kPrefix = "moref=";
    return vm.starts_with(kPrefix) ? vm.substr(kPrefix.size()) : vm;
}

struct DatastorePath {
    std::string datastore;
    std::string relative; // "dir/name.vmdk"
};

// "[datastore] dir/name.vmdk"; nullopt for anything else.
std::optional<DatastorePath> parseDatastorePath(const std::string& path)
{
    if (path.empty() || path.front() != '[') {
        return std::nullopt;
    }
    const auto close = path.find(']');
    if (close == std::string::npos || close == 1) {
        return std::nullopt;
    }
    DatastorePath parsed{path.substr(1, close - 1), path.substr(close + 1)};
    parsed.relative.erase(0, parsed.relative.find_first_not_of(' '));
    if (parsed.relative.empty() || parsed.relative.back() == '/') {
        return std::nullopt;
    }
    return parsed;
}

bool checkConnection(const Soap::ConnectParams& params)
{
    if (params.host.empty() || params.user.empty()) {
        std::cerr << "Error: a SOAP disk needs a host and a user\n";
        return false;
    }
    return true;
}

} // namespace

// --- Reading --------------------------------------------------------------------------------------------------------

class Soap::DiskReader::Impl
{
public:
    explicit Impl(std::unique_ptr<VSphereSoap::VmwareSoapDisk> disk) : m_disk(std::move(disk))
    {
        m_geometry.sectorSize = kSectorSize;
        m_geometry.phySectorSize = kSectorSize;
        m_geometry.sectors = m_disk->virtualSize() / kSectorSize;
    }

    const DiskGeometry& geometry() const { return m_geometry; }

    bool dataRanges(std::vector<ByteRange>& ranges)
    {
        ranges.clear();
        for (const auto& extent : m_disk->dataExtents()) {
            ranges.push_back(ByteRange{extent.offset, extent.length});
        }
        return true;
    }

    bool read(uint64_t offset, uint32_t length, void* pBuf)
    {
        auto* out = static_cast<std::byte*>(pBuf);
        uint32_t done = 0;
        while (done < length) {
            auto result = m_disk->readAt(offset + done, std::span(out + done, length - done));
            if (!result) {
                return failed("failed to read " + std::to_string(length) + " bytes at " + std::to_string(offset),
                              result.error());
            }
            if (result.value() == 0) {
                std::cerr << "Error: read at " << offset + done << " is past the end of the disk\n";
                return false;
            }
            done += static_cast<uint32_t>(result.value());
        }
        return true;
    }

    const std::string& nextChangeId() const { return m_disk->changeId(); }

private:
    std::unique_ptr<VSphereSoap::VmwareSoapDisk> m_disk;
    DiskGeometry m_geometry;
};

Soap::DiskReader::DiskReader() = default;
Soap::DiskReader::~DiskReader() = default;

bool Soap::DiskReader::open(const ConnectParams& params, const std::string& diskPath, const std::string& changeId)
{
    m_impl.reset();
    if (!checkConnection(params)) {
        return false;
    }
    if (params.vm.empty()) {
        std::cerr << "Error: reading a disk over SOAP needs its VM\n";
        return false;
    }
    const auto path = parseDatastorePath(diskPath);
    if (!path) {
        std::cerr << "Error: '" << diskPath << "' is not a \"[datastore] dir/name.vmdk\" path\n";
        return false;
    }
    routeLibraryLog();

    VSphereSoap::ConnectParams library = toLibrary(params);
    library.cbtChangeId = changeId;
    auto disk = VSphereSoap::VmwareSoapDisk::open(library, VSphereSoap::VmLocator::byMoref(vmMoref(params.vm)),
                                                  VSphereSoap::DiskSelector::byDatastorePath(path->relative));
    if (!disk) {
        return failed("failed to open '" + diskPath + "' over SOAP", disk.error());
    }
    if (!disk.value()->cbtActive() && !disk.value()->cbtUnavailableReason().empty()) {
        std::cerr << "Warning: no changed block tracking, so the whole disk is read: "
                  << disk.value()->cbtUnavailableReason() << "\n";
    }
    m_impl = std::make_unique<Impl>(std::move(disk).value());
    return true;
}

ABR_DISK_FORWARD_READER(Soap::DiskReader)

const std::string& Soap::DiskReader::nextChangeId() const
{
    static const std::string none;
    return m_impl ? m_impl->nextChangeId() : none;
}

// --- Writing --------------------------------------------------------------------------------------------------------

class Soap::Detail::WriterImpl
{
public:
    virtual ~WriterImpl() = default;

    virtual bool creates() const = 0;
    virtual const DiskGeometry& geometry() const = 0;
    virtual bool setImageSize(uint64_t byteSize) = 0;
    virtual bool write(uint64_t offset, uint32_t length, const void* pBuf) = 0;
    virtual bool finish() = 0;

protected:
    // Writes have to ascend for both uploads; caught here with a message that says so.
    bool checkAscending(uint64_t offset)
    {
        if (offset < m_written) {
            std::cerr << "Error: a SOAP restore writes front to back, but a write at " << offset
                      << " comes after one that ended at " << m_written << "\n";
            return false;
        }
        return true;
    }

    uint64_t m_written = 0;
};

namespace {

// An existing disk of a powered-off VM, its flat file replaced by one PUT.
class OverwriteWriter final : public Soap::Detail::WriterImpl
{
public:
    OverwriteWriter(VSphereSoap::ConnectParams params, std::string vm, std::string diskPath,
                    VSphereSoap::VSphereFolderPutPrepResult prepared,
                    std::unique_ptr<VSphereSoap::VSphereFolderPutBackend> backend)
        : m_params(std::move(params)), m_vm(std::move(vm)), m_diskPath(std::move(diskPath)),
          m_prepared(std::move(prepared)), m_backend(std::move(backend))
    {
        m_geometry.sectorSize = kSectorSize;
        m_geometry.phySectorSize = kSectorSize;
        m_geometry.sectors = m_backend->virtualSize() / kSectorSize;
    }

    ~OverwriteWriter() override
    {
        if (!m_finished) {
            std::cerr << "Warning: '" << m_diskPath
                      << "' was only partly overwritten; restore it again before using it\n";
        }
    }

    bool creates() const override { return false; }
    const DiskGeometry& geometry() const override { return m_geometry; }

    bool setImageSize(uint64_t byteSize) override
    {
        if (byteSize != m_backend->virtualSize()) {
            std::cerr << "Error: the backup holds " << byteSize << " bytes but '" << m_diskPath << "' has "
                      << m_backend->virtualSize() << "\n";
            return false;
        }
        return true;
    }

    bool write(uint64_t offset, uint32_t length, const void* pBuf) override
    {
        if (!checkAscending(offset)) {
            return false;
        }
        auto written = m_backend->writeAt(offset, std::span(static_cast<const std::byte*>(pBuf), length));
        if (!written) {
            return failed("failed to write at " + std::to_string(offset) + " of '" + m_diskPath + "'", written.error());
        }
        m_written = offset + length;
        return true;
    }

    bool finish() override
    {
        if (VSphereSoap::Error flushed = m_backend->flush(); !flushed.ok()) {
            return failed("failed to finish overwriting '" + m_diskPath + "'", flushed);
        }
        m_backend.reset();
        m_prepared = VSphereSoap::VSphereFolderPutPrepResult(); // logs the upload's session out
        m_finished = true;

        if (VSphereSoap::Error reset = VSphereSoap::finishFolderPutOverwrite(m_params,
                                                                             VSphereSoap::VmLocator::byMoref(m_vm));
            !reset.ok()) {
            return failed("'" + m_diskPath + "' was restored, but resetting the VM's change tracking failed; do not "
                          "take an incremental of it before a full backup",
                          reset);
        }
        return true;
    }

private:
    VSphereSoap::ConnectParams m_params;
    std::string m_vm;
    std::string m_diskPath;
    VSphereSoap::VSphereFolderPutPrepResult m_prepared; // keeps the PUT's session logged in
    std::unique_ptr<VSphereSoap::VSphereFolderPutBackend> m_backend;
    DiskGeometry m_geometry;
    bool m_finished = false;
};

// A new disk, uploaded into a temporary VM and then attached to the target VM or placed at its path.
class ImportWriter final : public Soap::Detail::WriterImpl
{
public:
    struct Destination {
        std::string vm;               // empty places the disk at path, unattached
        std::string fileName;         // the new disk's name with a VM, when given without a path
        std::optional<DatastorePath> path;
    };

    ImportWriter(VSphereSoap::ConnectParams params, Destination destination, uint64_t sizeBytes,
                 std::unique_ptr<VSphereSoap::VSphereImportSession> session,
                 std::unique_ptr<VSphereSoap::VSphereDatastoreBackend> backend)
        : m_params(std::move(params)), m_destination(std::move(destination)), m_sizeBytes(sizeBytes),
          m_session(std::move(session)), m_backend(std::move(backend))
    {
        m_session->startKeepAlive([this] { return m_percent.load(); });
    }

    ~ImportWriter() override
    {
        m_session->stopKeepAlive();
        if (!m_completed) {
            m_backend.reset(); // aborts the upload
            if (VSphereSoap::Error removed = m_session->destroyPlaceholderVm(); !removed.ok()) {
                std::cerr << "Warning: the temporary VM " << m_session->vmId()
                          << " of a failed restore could not be removed: " << removed.message() << "\n";
            }
        }
    }

    bool creates() const override { return true; }

    const DiskGeometry& geometry() const override { return Disk::Detail::noGeometry(); }

    bool setImageSize(uint64_t byteSize) override
    {
        if (byteSize > m_backend->virtualSize()) {
            std::cerr << "Error: the backup holds " << byteSize << " bytes, more than the " << m_backend->virtualSize()
                      << " the new disk was created with\n";
            return false;
        }
        return true;
    }

    bool write(uint64_t offset, uint32_t length, const void* pBuf) override
    {
        if (!checkAscending(offset)) {
            return false;
        }
        // The upload skips whole grains only, so a jump that starts or ends inside one has the rest of that grain
        // written as zeros.
        const uint64_t grain = m_grainBytes;
        if (offset > m_written) {
            if (m_written % grain != 0 && !writeZeros(m_written, std::min(offset, (m_written / grain + 1) * grain))) {
                return false;
            }
            const uint64_t grainStart = offset / grain * grain;
            if (offset % grain != 0 && !writeZeros(std::max(grainStart, m_written), offset)) {
                return false;
            }
        }
        if (!send(offset, std::span(static_cast<const std::byte*>(pBuf), length))) {
            return false;
        }
        m_percent = static_cast<int>(std::min<uint64_t>(99, m_written * 100 / std::max<uint64_t>(1, m_sizeBytes)));
        if (VSphereSoap::Error alive = m_backend->keepAliveIfIdle(m_written); !alive.ok()) {
            return failed("failed to keep the upload going", alive);
        }
        return true;
    }

    bool finish() override
    {
        if (VSphereSoap::Error flushed = m_backend->flush(); !flushed.ok()) {
            return failed("failed to finish uploading the new disk", flushed);
        }
        m_session->stopKeepAlive();
        if (VSphereSoap::Error completed = m_session->complete(); !completed.ok()) {
            return failed("failed to complete the upload of the new disk", completed);
        }
        m_completed = true; // from here on the disk is kept: a failure below says where it is

        if (m_destination.vm.empty()) {
            const std::string target = "[" + m_destination.path->datastore + "] " + m_destination.path->relative;
            auto released = VSphereSoap::releaseImportedDisk(m_params, m_session->vmId(), target, "thin");
            if (!released) {
                return failed("the disk was uploaded, but placing it at '" + target + "' failed", released.error());
            }
            return true;
        }

        VSphereSoap::VSphereAttachSpec spec;
        spec.placeholderVmId = m_session->vmId();
        spec.targetVm = VSphereSoap::VmLocator::byMoref(m_destination.vm);
        spec.diskProvisioning = "thin";
        if (m_destination.path) {
            const auto slash = m_destination.path->relative.find_last_of('/');
            spec.desiredFileName = slash == std::string::npos ? m_destination.path->relative
                                                              : m_destination.path->relative.substr(slash + 1);
            const std::string dir =
                slash == std::string::npos ? "" : " " + m_destination.path->relative.substr(0, slash);
            spec.desiredDatastoreDir = "[" + m_destination.path->datastore + "]" + dir;
        } else {
            spec.desiredFileName = m_destination.fileName;
        }
        auto attached = VSphereSoap::attachImportedDisk(m_params, spec);
        if (!attached) {
            return failed("the disk was uploaded, but attaching it to " + m_destination.vm + " failed",
                          attached.error());
        }
        std::cerr << "Note: the new disk is " << attached.value().datastorePath << "\n";
        return true;
    }

private:
    bool send(uint64_t offset, std::span<const std::byte> data)
    {
        auto written = m_backend->writeAt(offset, data);
        if (!written) {
            return failed("failed to upload at " + std::to_string(offset), written.error());
        }
        m_written = offset + data.size();
        return true;
    }

    bool writeZeros(uint64_t from, uint64_t to)
    {
        if (to <= from) {
            return true;
        }
        m_zeros.assign(to - from, std::byte{0});
        return send(from, m_zeros);
    }

    VSphereSoap::ConnectParams m_params;
    Destination m_destination;
    uint64_t m_sizeBytes = 0;
    uint64_t m_grainBytes = 128 * kSectorSize; // the upload's grain
    std::unique_ptr<VSphereSoap::VSphereImportSession> m_session;
    std::unique_ptr<VSphereSoap::VSphereDatastoreBackend> m_backend;
    std::vector<std::byte> m_zeros;
    std::atomic<int> m_percent{0};
    bool m_completed = false;
};

// The VM's datastore files: its disks, and the datastore its configuration is on.
struct VmFiles {
    std::vector<std::string> disks; // "[datastore] dir/name.vmdk"
    std::string datastore;
    std::string dir; // the VM's folder on that datastore
};

std::optional<VmFiles> describeVm(const VSphereSoap::ConnectParams& params, const std::string& vm)
{
    VSphereSoap::Vim::HttpTransportAdapter transport(params.host, params.port, params.verifyTlsCertificate,
                                                     params.pinnedThumbprint);
    VSphereSoap::Vim::SoapClient soap(transport, params.host);
    auto session = VSphereSoap::Vim::VimSession::open(soap, params.username, params.password);
    if (!session) {
        failed("failed to log in to " + params.host, session.error());
        return std::nullopt;
    }
    VSphereSoap::Vim::VimInventory inventory(*session.value());
    auto info = inventory.describeVm(VSphereSoap::Vim::MoRef{"VirtualMachine", vm});
    if (!info) {
        failed("failed to look up VM " + vm, info.error());
        return std::nullopt;
    }
    VmFiles files;
    for (const auto& disk : info.value().disks) {
        files.disks.push_back(disk.rawFileName);
    }
    if (const auto parsed = parseDatastorePath(info.value().vmPathName)) {
        files.datastore = parsed->datastore;
        const auto slash = parsed->relative.find_last_of('/');
        files.dir = slash == std::string::npos ? std::string() : parsed->relative.substr(0, slash);
    }
    return files;
}

// A new disk is moved to its final path only after the whole upload, and a taken path fails it there, so the path
// is checked before anything is sent.
bool destinationFree(const VSphereSoap::ConnectParams& params, const std::string& datastore, const std::string& dir,
                     std::string name)
{
    if (!name.ends_with(".vmdk")) {
        name += ".vmdk";
    }
    const std::string target = "[" + datastore + "] " + (dir.empty() ? name : dir + "/" + name);
    auto exists = VSphereSoap::datastoreDiskExists(params, target);
    if (!exists) {
        return failed("cannot check whether '" + target + "' exists", exists.error());
    }
    if (exists.value()) {
        std::cerr << "Error: '" << target << "' already exists; restore to another name\n";
        return false;
    }
    return true;
}

std::unique_ptr<Soap::Detail::WriterImpl> openOverwrite(const VSphereSoap::ConnectParams& params, const std::string& vm,
                                                      const std::string& diskPath, const DatastorePath& path)
{
    auto prepared = VSphereSoap::prepareFolderPutOverwrite(params, VSphereSoap::VmLocator::byMoref(vm),
                                                           VSphereSoap::DiskSelector::byDatastorePath(path.relative));
    if (!prepared) {
        failed("cannot overwrite '" + diskPath + "'", prepared.error());
        return nullptr;
    }
    VSphereSoap::VSphereFolderPutBackendParams backendParams;
    backendParams.putUrl = prepared.value().flatPutUrl;
    backendParams.verifyTlsCertificate = params.verifyTlsCertificate;
    backendParams.pinnedThumbprint = params.pinnedThumbprint;
    backendParams.sessionCookie = prepared.value().sessionCookie;
    backendParams.virtualSizeBytes = prepared.value().virtualSizeBytes;
    auto backend = VSphereSoap::VSphereFolderPutBackend::open(backendParams);
    if (!backend) {
        failed("cannot start overwriting '" + diskPath + "'", backend.error());
        return nullptr;
    }
    return std::make_unique<OverwriteWriter>(params, vm, diskPath, std::move(prepared).value(),
                                             std::move(backend).value());
}

std::unique_ptr<Soap::Detail::WriterImpl> openImport(const Soap::ConnectParams& soapParams,
                                                   const VSphereSoap::ConnectParams& params,
                                                   ImportWriter::Destination destination, const std::string& datastore,
                                                   uint64_t sizeBytes)
{
    VSphereSoap::VSpherePlacementSpec placementSpec;
    placementSpec.datastoreName = datastore;
    placementSpec.computeName = soapParams.compute;
    placementSpec.resourcePoolName = soapParams.resourcePool;
    auto placement = VSphereSoap::resolvePlacement(params, placementSpec);
    if (!placement) {
        failed("cannot place the temporary VM for the new disk on datastore '" + datastore + "'", placement.error());
        return nullptr;
    }

    const uint64_t capacity = VSphereSoap::roundUpToGrain(sizeBytes);
    if (capacity != sizeBytes) {
        std::cerr << "Warning: the new disk is " << capacity << " bytes, the backup's " << sizeBytes
                  << " rounded up to whole 64 KiB grains\n";
    }

    VSphereSoap::VSphereImportVmSpec spec;
    spec.vmName = soapParams.identity + "-restore-" +
                  std::to_string(std::chrono::system_clock::now().time_since_epoch() / std::chrono::seconds(1));
    spec.memoryBytes = 128ull << 20;
    spec.cpuCount = 1;
    spec.resourcePoolId = placement.value().resourcePoolId;
    spec.folderId = placement.value().folderId;
    spec.datastoreId = placement.value().datastoreId;
    spec.diskProvisioning = "thin";
    spec.annotation = "Temporary VM of a disk restore; deleted when the restore ends.";
    spec.disks.push_back(VSphereSoap::VSphereImportDiskSpec{"disk-0", capacity});

    auto session = VSphereSoap::VSphereImportSession::begin(params, spec);
    if (!session) {
        failed("failed to create the temporary VM for the new disk", session.error());
        return nullptr;
    }

    VSphereSoap::VSphereDatastoreBackendParams backendParams;
    // The upload goes to the ESXi host the lease names, whose certificate is not the vCenter's: the lease says which
    // one to expect.
    const auto& deviceUrl = session.value()->deviceUrls().front();
    backendParams.deviceUrl = deviceUrl.url;
    backendParams.verifyTlsCertificate = params.verifyTlsCertificate;
    backendParams.pinnedThumbprint =
        deviceUrl.sslThumbprint.empty() ? params.pinnedThumbprint : deviceUrl.sslThumbprint;
    backendParams.virtualSizeBytes = capacity;
    backendParams.vmdkDescriptor = VSphereSoap::makeStreamOptimizedDescriptor(capacity, "disk.vmdk");
    backendParams.compressGrains = true;
    auto backend = VSphereSoap::VSphereDatastoreBackend::open(backendParams);
    if (!backend) {
        failed("failed to start uploading the new disk", backend.error());
        (void)session.value()->destroyPlaceholderVm();
        return nullptr;
    }
    return std::make_unique<ImportWriter>(params, std::move(destination), sizeBytes, std::move(session).value(),
                                          std::move(backend).value());
}

bool pathFree(const VSphereSoap::ConnectParams& params, const DatastorePath& path)
{
    const auto slash = path.relative.find_last_of('/');
    if (slash == std::string::npos) {
        return destinationFree(params, path.datastore, {}, path.relative);
    }
    return destinationFree(params, path.datastore, path.relative.substr(0, slash), path.relative.substr(slash + 1));
}

} // namespace

Soap::DiskWriter::DiskWriter() = default;
Soap::DiskWriter::~DiskWriter() = default;

bool Soap::DiskWriter::open(const ConnectParams& params, const std::string& vmdk, uint64_t sizeBytes)
{
    m_impl.reset();
    if (!checkConnection(params)) {
        return false;
    }
    const auto path = parseDatastorePath(vmdk);
    if (params.vm.empty() && !path) {
        std::cerr << "Error: a disk restored without a VM needs a \"[datastore] dir/name.vmdk\" path, not '" << vmdk
                  << "'\n";
        return false;
    }
    if (!path && (vmdk.empty() || vmdk.find('/') != std::string::npos || vmdk.find('[') != std::string::npos)) {
        std::cerr << "Error: '" << vmdk << "' is neither a file name nor a \"[datastore] dir/name.vmdk\" path\n";
        return false;
    }
    routeLibraryLog();

    const VSphereSoap::ConnectParams library = toLibrary(params);

    if (params.vm.empty()) {
        if (sizeBytes == 0 || sizeBytes % kSectorSize != 0) {
            std::cerr << "Error: a new disk needs a size in whole sectors, not " << sizeBytes << " bytes\n";
            return false;
        }
        if (!pathFree(library, *path)) {
            return false;
        }
        m_impl = openImport(params, library, ImportWriter::Destination{{}, {}, path}, path->datastore, sizeBytes);
        return m_impl != nullptr;
    }

    const std::string vm = vmMoref(params.vm);
    const auto files = describeVm(library, vm);
    if (!files) {
        return false;
    }
    if (path) {
        const std::string normalized = "[" + path->datastore + "] " + path->relative;
        if (std::find(files->disks.begin(), files->disks.end(), normalized) != files->disks.end()) {
            m_impl = openOverwrite(library, vm, vmdk, *path);
            return m_impl != nullptr;
        }
    }
    if (sizeBytes == 0 || sizeBytes % kSectorSize != 0) {
        std::cerr << "Error: a new disk needs a size in whole sectors, not " << sizeBytes << " bytes\n";
        return false;
    }
    if (path ? !pathFree(library, *path) : !destinationFree(library, files->datastore, files->dir, vmdk)) {
        return false;
    }
    const std::string datastore = path ? path->datastore : files->datastore;
    m_impl = openImport(params, library, ImportWriter::Destination{vm, path ? std::string() : vmdk, path}, datastore,
                        sizeBytes);
    return m_impl != nullptr;
}

bool Soap::DiskWriter::creates() const
{
    return m_impl && m_impl->creates();
}

ABR_DISK_FORWARD_WRITER(Soap::DiskWriter)
