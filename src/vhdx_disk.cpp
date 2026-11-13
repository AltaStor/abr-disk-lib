// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Hyper-V virtual disk (VHD/VHDX) source/target
//
// A virtual disk is reached through the Windows virtual disk service, the same
// one Hyper-V itself uses: it is opened and queried through virtdisk.dll, then
// attached -- surfaced as a disk device of the host -- and read or written
// through that device. 
// Attaching is what turns a VHDX, a differencing chain of AVHDXs, or a VHD into
// the plain run of sectors the guest sees, so the library never has to
// understand any of those formats itself.
//
#include "vhdx_disk.h"

#include <windows.h>

#include <sddl.h>
#include <winnetwk.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <iostream>

#include "disk_detail.h"
#include "virtdisk_api.h"
#include "win_disk.h"
#include "win_util.h"

namespace {

// virtdisk.h declares these through DEFINE_GUID, which only defines them in a
// translation unit that includes initguid.h first -- and then in every one
// that does. Values of their own sidestep both the link error and the clash.
constexpr GUID kVendorUnknown = {0x00000000, 0x0000, 0x0000, {0, 0, 0, 0, 0, 0, 0, 0}};
constexpr GUID kVendorMicrosoft = {0xEC984AEC, 0xA0F9, 0x47E9, {0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B}};

// What GET_VIRTUAL_DISK_INFO_PROVIDER_SUBTYPE answers with.
constexpr ULONG kSubtypeFixed = 2;
constexpr ULONG kSubtypeDynamic = 3;
constexpr ULONG kSubtypeDifferencing = 4;

// Who may open the disk device a virtual disk is attached as: the system and administrators.
// Left to itself the attach would take the image file's ACL, which on a Hyper-V host grants
// the VM's own account and whatever the folder passes down -- and anyone the device is open
// to can read the guest's disk.
constexpr const wchar_t* kAttachedDiskSddl = L"O:BAG:BAD:(A;;GA;;;SY)(A;;GA;;;BA)";

// The attach returns once the virtual disk service has the disk, and the disk
// device appears a moment later, once Plug and Play has built it.
constexpr DWORD kSurfaceTimeoutMs = 15000;
constexpr DWORD kSurfacePollMs = 100;

// How many changed ranges one QueryChangesVirtualDisk call may hand back. The
// call stops early when they run out and says how far it got, so this only
// sets how many calls a heavily changed disk takes.
constexpr ULONG kRangesPerQuery = 4096;

const char* const kInUseForBackup =
    "The disk is in use, most likely by a running virtual machine. Back up a checkpoint\n"
    "of the VM instead: once it has one, the disk the checkpoint refers to is no longer\n"
    "written to and can be read while the VM runs.\n";

const char* const kInUseForRestore =
    "The disk is in use -- is its virtual machine running? A disk can only be restored\n"
    "to while nothing else has it open.\n";

// The virtual disk entry points, bound by VirtDisk::loadLibrary. Nothing in
// this file may call through them before openReader or openWriter has done so.
const VirtDisk::Api& virtdisk()
{
    return VirtDisk::api();
}

// virtdisk.dll hands its failures back as the return value, not through GetLastError.
void reportFailure(const std::string& what, DWORD error)
{
    std::cerr << "Error: " << what << ": " << Win::errorText(error) << "\n";
}

std::string lowerExtension(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    const size_t dot = path.find_last_of('.');

    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return {};
    }

    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// Reads `length` bytes at `offset` of a file; false when it could not.
bool readHead(HANDLE file, uint64_t offset, char* pBuf, uint32_t length)
{
    return Win::readAt(file, offset, pBuf, length) == ERROR_SUCCESS;
}

// The format of an image, told by its name the way Hyper-V tells it, and
// failing that by what the file holds: a VHDX starts with "vhdxfile", a VHD
// ends -- and, when it is dynamic, also starts -- with a "conectix" footer.
// Left undecided, the virtual disk service goes by the name alone and turns
// down one it does not know.
ULONG deviceTypeOf(const std::string& path)
{
    const std::string extension = lowerExtension(path);

    if (extension == ".vhdx" || extension == ".avhdx") {
        return VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    }
    if (extension == ".vhd" || extension == ".avhd") {
        return VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    }

    Win::Handle file(CreateFileW(Win::widen(path).c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, 0, nullptr));
    LARGE_INTEGER size = {};
    char signature[8] = {};

    if (!file.valid() || !GetFileSizeEx(file.get(), &size) || size.QuadPart < 512) {
        return VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
    }

    if (readHead(file.get(), 0, signature, sizeof(signature))) {
        if (std::memcmp(signature, "vhdxfile", sizeof(signature)) == 0) {
            return VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        }
        if (std::memcmp(signature, "conectix", sizeof(signature)) == 0) {
            return VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
        }
    }

    if (readHead(file.get(), static_cast<uint64_t>(size.QuadPart) - 512, signature,
                 sizeof(signature))
        && std::memcmp(signature, "conectix", sizeof(signature)) == 0) {
        return VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    }
    return VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
}

const char* formatName(ULONG deviceId)
{
    switch (deviceId) {
        case VIRTUAL_STORAGE_TYPE_DEVICE_VHD:  return "VHD";
        case VIRTUAL_STORAGE_TYPE_DEVICE_VHDX: return "VHDX";
        case VIRTUAL_STORAGE_TYPE_DEVICE_ISO:  return "ISO";
        default:                               return "unknown";
    }
}

const char* subtypeName(ULONG subtype)
{
    switch (subtype) {
        case kSubtypeFixed:        return "fixed";
        case kSubtypeDynamic:      return "dynamic";
        case kSubtypeDifferencing: return "differencing";
        default:                   return "unknown";
    }
}

// Both reading and writing a virtual disk go through attaching it, and the
// attach is closed to anyone without a full administrator token. Checked up
// front, so that a backup that cannot work says why before it opens anything.
bool checkElevated(const std::string& path)
{
    if (Win::isElevated()) {
        return true;
    }
    std::cerr << "Error: '" << path << "' has to be attached to be read or written, "
              << "which needs an elevated (administrator) process\n";
    return false;
}

class VirtualDisk : public IDiskReader, public IDiskWriter
{
public:
    ~VirtualDisk() override { release(); }

    bool open(const std::string& path, bool readOnly)
    {
        m_path = path;
        m_readOnly = readOnly;

        const ULONG deviceId = deviceTypeOf(path);
        VIRTUAL_STORAGE_TYPE storageType = {
            deviceId,
            deviceId == VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN ? kVendorUnknown : kVendorMicrosoft
        };

        // Version 2 opens take their access from ReadOnly rather than from an
        // access mask, which has to be left at NONE, and the resulting handle
        // may be attached, queried and asked for changes alike.
        OPEN_VIRTUAL_DISK_PARAMETERS parameters = {};
        parameters.Version = OPEN_VIRTUAL_DISK_VERSION_2;
        parameters.Version2.GetInfoOnly = FALSE;
        parameters.Version2.ReadOnly = readOnly ? TRUE : FALSE;

        const DWORD error = virtdisk().openVirtualDisk(&storageType, Win::widen(path).c_str(),
                                                       VIRTUAL_DISK_ACCESS_NONE,
                                                       OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters,
                                                       m_handle.put());
        if (error != ERROR_SUCCESS) {
            // Whatever the call left behind is not a handle to close.
            m_handle.release();

            reportFailure("failed to open virtual disk '" + path + "'", error);
            if (error == ERROR_SHARING_VIOLATION) {
                std::cerr << (readOnly ? kInUseForBackup : kInUseForRestore);
            }
            return false;
        }

        return readGeometry();
    }

    bool create(const std::string& path, const DiskGeometry& image)
    {
        const std::string extension = lowerExtension(path);
        ULONG deviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;

        if (extension == ".vhdx") {
            deviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        } else if (extension == ".vhd") {
            deviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
        } else {
            // Hyper-V will not attach an image to a VM under any other name, so
            // guessing would only defer the failure to a worse moment.
            std::cerr << "Error: cannot tell from its name whether to create '" << path
                      << "' as a VHDX or a VHD; name it .vhdx (or .vhd)\n";
            return false;
        }

        // A VHD knows only 512-byte sectors; a VHDX knows 512 and 4096.
        if ((image.sectorSize != 512 && image.sectorSize != 4096)
            || (deviceId == VIRTUAL_STORAGE_TYPE_DEVICE_VHD && image.sectorSize != 512)) {
            std::cerr << "Error: a " << formatName(deviceId) << " cannot hold the backup's "
                      << image.sectorSize << "-byte sectors"
                      << (deviceId == VIRTUAL_STORAGE_TYPE_DEVICE_VHD
                              ? "; restore it to a .vhdx instead\n"
                              : "\n");
            return false;
        }

        const uint64_t size = image.sectors * image.sectorSize;

        CREATE_VIRTUAL_DISK_PARAMETERS parameters = {};
        parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        parameters.Version2.MaximumSize = size;
        parameters.Version2.BlockSizeInBytes = 0; // the format's default
        parameters.Version2.SectorSizeInBytes = image.sectorSize;

        // Only a VHDX records a physical sector size of its own; anything the
        // backup reports other than the two it knows is left to its default.
        if (deviceId == VIRTUAL_STORAGE_TYPE_DEVICE_VHDX
            && (image.phySectorSize == 512 || image.phySectorSize == 4096)) {
            parameters.Version2.PhysicalSectorSizeInBytes = image.phySectorSize;
        }

        VIRTUAL_STORAGE_TYPE storageType = {deviceId, kVendorMicrosoft};
        Win::Handle created;

        // No flags means dynamic: the file grows as the restore writes to it,
        // like the sparse file a plain restore target is.
        const DWORD error = virtdisk().createVirtualDisk(
            &storageType, Win::widen(path).c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr,
            CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &parameters, nullptr, created.put());
        if (error != ERROR_SUCCESS) {
            created.release();
            reportFailure("failed to create virtual disk '" + path + "' of " + std::to_string(size) + " bytes", error);
            return false;
        }

        // From here on the file is this run's to remove if the restore does
        // not finish.
        m_path = path;
        m_created = true;
        created.close();

        std::cerr << "Created dynamic " << formatName(deviceId) << " '" << path << "' of " << size << " bytes\n";

        // Reopened rather than attached through the handle the create returned,
        // which is the path every other disk takes.
        return open(path, false);
    }

    void readProperties(Vhdx::DiskProperties& properties)
    {
        GET_VIRTUAL_DISK_INFO info = {};
        ULONG size = sizeof(info);

        info.Version = GET_VIRTUAL_DISK_INFO_VIRTUAL_STORAGE_TYPE;
        if (virtdisk().getVirtualDiskInformation(m_handle.get(), &size, &info, nullptr) == ERROR_SUCCESS) {
            properties.format = formatName(info.VirtualStorageType.DeviceId);
        }

        info = {};
        size = sizeof(info);
        info.Version = GET_VIRTUAL_DISK_INFO_PROVIDER_SUBTYPE;
        if (virtdisk().getVirtualDiskInformation(m_handle.get(), &size, &info, nullptr) == ERROR_SUCCESS) {
            properties.type = subtypeName(info.ProviderSubtype);
        }

        properties.changeTrackingId = readChangeTrackingId();
        m_changeTracking = !properties.changeTrackingId.empty();
    }

    // Works out what changed since changeId, when the disk is opened rather
    // than when the copy asks for its ranges: a change id the disk does not
    // know then fails the backup before anything is attached or written.
    bool collectChanges(const std::string& changeId)
    {
        if (!virtdisk().queryChangesVirtualDisk) {
            std::cerr << "Error: this version of Windows has no resilient change tracking, "
                      << "which incremental backups need (Windows 10 1607 or Windows Server "
                      << "2016 and later)\n";
            return false;
        }

        if (!m_changeTracking) {
            std::cerr << "Error: resilient change tracking is not enabled on '" << m_path
                      << "', so nothing can tell what changed since '" << changeId
                      << "'; take a full backup instead\n";
            return false;
        }

        const std::wstring id = Win::widen(changeId);
        std::vector<QUERY_CHANGES_VIRTUAL_DISK_RANGE> batch(kRangesPerQuery);
        uint64_t offset = 0;

        while (offset < m_virtualSize) {
            ULONG count = kRangesPerQuery;
            ULONG64 processed = 0;

            const DWORD error = virtdisk().queryChangesVirtualDisk(
                m_handle.get(), id.c_str(), offset, m_virtualSize - offset,
                QUERY_CHANGES_VIRTUAL_DISK_FLAG_NONE, batch.data(), &count, &processed);
            if (error != ERROR_SUCCESS) {
                reportFailure("failed to query what changed on '" + m_path + "' since change id '"
                                  + changeId + "'", error);
                std::cerr << "A change id is only good for the disk that issued it, and only as\n"
                          << "long as its tracking lasts: a disk that was moved, merged or\n"
                          << "replaced starts over. Take a full backup to start a new chain.\n";
                return false;
            }

            for (ULONG i = 0; i < count; ++i) {
                addChange(batch[i].ByteOffset, batch[i].ByteLength);
            }

            // A reply that does not move past the offset asked about would spin
            // here forever. Taking it for the end of the disk would quietly
            // leave the rest out of the incremental, so it is a failure.
            if (processed == 0) {
                std::cerr << "Error: change tracking of '" << m_path << 
                        "' stopped reporting at offset " << offset << " of " << m_virtualSize << "\n";
                return false;
            }
            offset += processed;
        }

        mergeChanges();
        m_incremental = true;
        return true;
    }

    bool attach()
    {
        // Administrators hold the privilege attaching needs, but it starts out
        // disabled. Should it be missing, the attach itself says so.
        Win::enablePrivilege(L"SeManageVolumePrivilege");

        PSECURITY_DESCRIPTOR pSecurity = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kAttachedDiskSddl, SDDL_REVISION_1, &pSecurity, nullptr)) {
            std::cerr << "Error: failed to build the security descriptor of the attached disk: "
                      << Win::errorText(GetLastError()) << "\n";
            return false;
        }

        ATTACH_VIRTUAL_DISK_PARAMETERS parameters = {};
        parameters.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

        // No drive letters, since nothing on the disk is meant to be browsed.
        // Not permanent either: the attachment lasts as long as the handle does,
        // so a run that is killed outright leaves no disk attached behind it.
        auto flags = ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
        if (m_readOnly) {
            flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(flags | ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY);
        }

        const DWORD error = virtdisk().attachVirtualDisk(m_handle.get(), pSecurity, flags, 0,
                                                         &parameters, nullptr);
        LocalFree(pSecurity);

        if (error != ERROR_SUCCESS) {
            reportFailure("failed to attach virtual disk '" + m_path + "'", error);

            // The open shares the image with a running VM, even for writing, so
            // it is here that one holding the disk is found out.
            if (error == ERROR_SHARING_VIOLATION) {
                std::cerr << (m_readOnly ? kInUseForBackup : kInUseForRestore);
            }
            return false;
        }
        m_attached = true;

        std::wstring physicalPath(MAX_PATH, L'\0');
        ULONG bytes = static_cast<ULONG>(physicalPath.size() * sizeof(wchar_t));
        DWORD pathError = virtdisk().getVirtualDiskPhysicalPath(m_handle.get(), &bytes, physicalPath.data());

        if (pathError == ERROR_INSUFFICIENT_BUFFER) {
            physicalPath.assign(bytes / sizeof(wchar_t) + 1, L'\0');
            bytes = static_cast<ULONG>(physicalPath.size() * sizeof(wchar_t));
            pathError = virtdisk().getVirtualDiskPhysicalPath(m_handle.get(), &bytes, physicalPath.data());
        }
        if (pathError != ERROR_SUCCESS) {
            reportFailure("failed to find the disk '" + m_path + "' is attached as", pathError);
            return false;
        }

        physicalPath.resize(wcsnlen(physicalPath.c_str(), physicalPath.size()));
        return openAttachedDisk(Win::narrow(physicalPath));
    }

    bool claimForWriting()
    {
        // Left offline: the disk is detached as soon as the restore is done,
        // and bringing it online first would only have Windows mount the
        // restored file systems -- and write to them -- on the way out.
        return m_disk.claim(false);
    }

    const DiskGeometry& geometry() const override { return m_geometry; }

    bool dataRanges(std::vector<ByteRange>& ranges) override
    {
        ranges.clear();

        if (m_incremental) {
            ranges = m_changes;
            return true;
        }

        // A full backup reads the whole disk. The virtual disk service has no call that
        // lists what a disk has allocated -- change tracking only answers relative
        // to an earlier id -- and working it out from the image would mean reading
        // VHDX and VHD structures, and every parent of a differencing chain, behind
        // the service's back. 
        // Unallocated blocks read as zeros, and backuplib drops zeroed blocks rather than
        // storing them, so they cost time but no space.
        if (m_virtualSize > 0) {
            ranges.push_back({0, m_virtualSize});
        }
        return true;
    }

    bool read(uint64_t offset, uint32_t length, void* pBuf) override
    {
        return m_disk.read(offset, length, pBuf);
    }

    bool write(uint64_t offset, uint32_t length, const void* pBuf) override
    {
        return m_disk.write(offset, length, pBuf);
    }

    // The disk already has the geometry it was created with; a restore cannot
    // change it, and whether the two agree is the caller's to check.
    bool setImageSize(uint64_t) override { return true; }

    bool finish() override
    {
        if (!m_disk.flush()) {
            return false;
        }

        // The restored data is in the image now. Anything that goes wrong
        // detaching it is worth a warning, but no reason to throw it away.
        m_finished = true;
        release();
        return true;
    }

private:
    bool readGeometry()
    {
        GET_VIRTUAL_DISK_INFO info = {};
        ULONG size = sizeof(info);
        info.Version = GET_VIRTUAL_DISK_INFO_SIZE;

        const DWORD error =
            virtdisk().getVirtualDiskInformation(m_handle.get(), &size, &info, nullptr);
        if (error != ERROR_SUCCESS) {
            reportFailure("failed to query the size of virtual disk '" + m_path + "'", error);
            return false;
        }

        m_virtualSize = info.Size.VirtualSize;
        m_geometry.sectorSize = info.Size.SectorSize;

        // Everything below converts between bytes and sectors by dividing, so
        // a disk that reports no sector size has to be rejected here.
        if (m_geometry.sectorSize == 0 || m_virtualSize % m_geometry.sectorSize != 0) {
            std::cerr << "Error: virtual disk '" << m_path << "' reports " << m_virtualSize
                      << " bytes in " << m_geometry.sectorSize << "-byte sectors\n";
            return false;
        }
        m_geometry.sectors = m_virtualSize / m_geometry.sectorSize;

        // A VHD may not answer this, and then its sectors are what it has.
        info = {};
        size = sizeof(info);
        info.Version = GET_VIRTUAL_DISK_INFO_VHD_PHYSICAL_SECTOR_SIZE;
        if (virtdisk().getVirtualDiskInformation(m_handle.get(), &size, &info, nullptr) == ERROR_SUCCESS) {
            m_geometry.phySectorSize = info.VhdPhysicalSectorSize;
        }
        if (m_geometry.phySectorSize == 0) {
            m_geometry.phySectorSize = m_geometry.sectorSize;
        }
        return true;
    }

    // The id comes back as a string of whatever length inside the structure,
    // so the buffer is grown to the size the call asks for.
    std::string readChangeTrackingId()
    {
        std::vector<uint8_t> buffer(sizeof(GET_VIRTUAL_DISK_INFO) + 256 * sizeof(wchar_t));
        DWORD error = ERROR_INSUFFICIENT_BUFFER;

        for (int attempt = 0; attempt < 2 && error == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
            auto* pInfo = reinterpret_cast<GET_VIRTUAL_DISK_INFO*>(buffer.data());
            pInfo->Version = GET_VIRTUAL_DISK_INFO_CHANGE_TRACKING_STATE;

            ULONG size = static_cast<ULONG>(buffer.size());
            error = virtdisk().getVirtualDiskInformation(m_handle.get(), &size, pInfo, nullptr);

            if (error == ERROR_INSUFFICIENT_BUFFER && size > buffer.size()) {
                buffer.assign(size, 0);
            }
        }

        // Not answering at all -- a VHD, a Windows that predates the query --
        // is the same as tracking being off.
        const auto* pInfo = reinterpret_cast<const GET_VIRTUAL_DISK_INFO*>(buffer.data());
        if (error != ERROR_SUCCESS || !pInfo->ChangeTrackingState.Enabled) {
            return {};
        }

        const wchar_t* pId = pInfo->ChangeTrackingState.MostRecentId;
        const size_t room = (buffer.data() + buffer.size() - reinterpret_cast<const uint8_t*>(pId)) / sizeof(wchar_t);

        return Win::narrow(pId, wcsnlen(pId, room));
    }

    // Clipped to the disk and widened to whole sectors, since the attached disk takes nothing less.
    void addChange(uint64_t offset, uint64_t length)
    {
        if (length == 0 || offset >= m_virtualSize) {
            return;
        }

        const uint64_t sector = m_geometry.sectorSize;
        const uint64_t begin = offset - offset % sector;
        uint64_t end = std::min(offset + length, m_virtualSize);
        end = std::min((end + sector - 1) / sector * sector, m_virtualSize);

        m_changes.push_back({begin, end - begin});
    }

    // Widening to sectors can make neighbours overlap, and a range written to
    // the backup twice is a range restored twice; one pass in offset order
    // folds every overlap and every touching pair into one range.
    void mergeChanges()
    {
        std::sort(m_changes.begin(), m_changes.end(),
                  [](const ByteRange& left, const ByteRange& right) {
                      return left.offset < right.offset;
                  });

        std::vector<ByteRange> merged;
        for (const auto& range : m_changes) {
            if (!merged.empty() && range.offset <= merged.back().offset + merged.back().length) {
                ByteRange& last = merged.back();
                last.length = std::max(last.offset + last.length, range.offset + range.length) - last.offset;
            } else {
                merged.push_back(range);
            }
        }
        m_changes.swap(merged);
    }

    bool openAttachedDisk(const std::string& path)
    {
        Win::Handle handle;
        DWORD error = ERROR_SUCCESS;

        for (DWORD waited = 0;; waited += kSurfacePollMs) {
            error = Win::openDevice(path, !m_readOnly, handle);
            if (error == ERROR_SUCCESS
                || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                || waited >= kSurfaceTimeoutMs) {
                break;
            }
            Sleep(kSurfacePollMs);
        }

        if (error != ERROR_SUCCESS) {
            reportFailure("failed to open '" + path + "', the disk '" + m_path + "' is attached as", error);
            return false;
        }

        if (!m_disk.adopt(std::move(handle), path, !m_readOnly)) {
            return false;
        }

        // Every offset the copy uses is an offset into the virtual disk, so the
        // device has to be that disk byte for byte.
        const DiskGeometry& device = m_disk.geometry();
        if (m_disk.size() != m_virtualSize || device.sectorSize != m_geometry.sectorSize) {
            std::cerr << "Error: '" << m_path << "' is attached as '" << path << "', which holds "
                      << m_disk.size() << " bytes in " << device.sectorSize
                      << "-byte sectors, but the virtual disk holds " << m_virtualSize
                      << " bytes in " << m_geometry.sectorSize << "-byte sectors\n";
            return false;
        }

        std::cerr << "Virtual disk '" << m_path << "' attached " << (m_readOnly ? "read-only " : "")
                  << "as " << path << "\n";
        return true;
    }

    // Closes the device, detaches the disk and closes it, in that order: the
    // detach waits on nothing else holding the device.
    void release()
    {
        m_disk.close();

        if (m_attached) {
            m_attached = false;

            // The attachment is tied to the handle, so closing the handle below
            // detaches the disk anyway; a failure here only means that happens
            // a moment later.
            const DWORD error = virtdisk().detachVirtualDisk(m_handle.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
            if (error != ERROR_SUCCESS) {
                std::cerr << "Warning: failed to detach virtual disk '" << m_path
                          << "': " << Win::errorText(error) << "\n";
            }
        }

        m_handle.close();

        // A disk this run created and did not finish holds part of a restore at
        // best. Nothing else knows it exists, so it goes, rather than be taken
        // later for the disk it was meant to become.
        if (m_created && !m_finished) {
            m_created = false;

            if (DeleteFileW(Win::widen(m_path).c_str())) {
                std::cerr << "Removed the unfinished virtual disk '" << m_path << "'\n";
            } else {
                std::cerr << "Warning: failed to remove the unfinished virtual disk '" << m_path
                          << "': " << Win::errorText(GetLastError()) << "\n";
            }
        }
    }

    std::string m_path;
    bool m_readOnly = true;

    Win::Handle m_handle;
    bool m_attached = false;
    Win::RawDisk m_disk;

    DiskGeometry m_geometry;
    uint64_t m_virtualSize = 0;

    bool m_changeTracking = false;
    bool m_incremental = false;
    std::vector<ByteRange> m_changes;

    bool m_created = false;
    bool m_finished = false;
};

// The \\server\share a UNC path starts with, which is what a connection is made
// to; empty for any other path. The long form \\?\UNC\server\share is the
// same share under another spelling.
std::wstring shareOf(const std::string& path)
{
    std::wstring wide = Win::widen(path);
    std::replace(wide.begin(), wide.end(), L'/', L'\\');

    const std::wstring longPrefix = L"\\\\?\\UNC\\";
    if (wide.size() > longPrefix.size()
        && _wcsnicmp(wide.c_str(), longPrefix.c_str(), longPrefix.size()) == 0) {
        wide = L"\\\\" + wide.substr(longPrefix.size());
    }

    // \\.\ and \\?\ name devices and local paths, not servers.
    if (wide.size() < 5 || wide[0] != L'\\' || wide[1] != L'\\' || wide[2] == L'.' || wide[2] == L'?'
        || wide[2] == L'\\') {
        return {};
    }

    const size_t server = wide.find(L'\\', 2);
    if (server == std::wstring::npos || server + 1 >= wide.size()) {
        return {};
    }

    const size_t share = wide.find(L'\\', server + 1);
    return share == std::wstring::npos ? wide : wide.substr(0, share);
}

//
// A connection to an SMB share made under credentials of the caller's choosing,
// held for as long as a disk on it is open.
//
// Not remembered (CONNECT_TEMPORARY) and given no drive letter: nothing of it
// is meant to outlast the run or to show up in anyone's Explorer. And only a
// connection this object made is dropped again -- one the host already had is
// somebody else's, and ending it would pull the share out from under them.
//
class ShareConnection
{
public:
    explicit ShareConnection(std::wstring share)
        : m_share(std::move(share))
    {}

    ~ShareConnection()
    {
        if (m_connected) {
            WNetCancelConnection2W(m_share.c_str(), 0, FALSE);
        }
    }

    ShareConnection(const ShareConnection&) = delete;
    ShareConnection& operator=(const ShareConnection&) = delete;

    bool connect(const Vhdx::ShareCredentials& credentials)
    {
        NETRESOURCEW resource = {};
        resource.dwType = RESOURCETYPE_DISK;
        resource.lpRemoteName = m_share.data();

        const std::wstring user = Win::widen(credentials.user);
        const std::wstring password = Win::widen(credentials.password);

        const DWORD error = WNetAddConnection2W(&resource, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
        if (error == ERROR_SUCCESS) {
            m_connected = true;
            return true;
        }

        std::cerr << "Error: failed to connect to '" << Win::narrow(m_share) << "' as '"
                  << credentials.user << "': " << Win::errorText(error) << "\n";

        // Windows keeps one set of credentials per server, so a session opened
        // under another account has to go before this one can be.
        if (error == ERROR_SESSION_CREDENTIAL_CONFLICT) {
            std::cerr << "This host is already connected to that server under another account;\n"
                      << "disconnect it first ('net use'), or leave the credentials out to use\n"
                      << "the connection that is there.\n";
        }
        return false;
    }

private:
    std::wstring m_share;
    bool m_connected = false;
};

// Connects the share a remote path is on when credentials were given for it.
// True with `share` left empty when there is nothing to connect: a local path,
// or a remote one the process's own logon is to be used for.
bool connectShare(const std::string& path, const Vhdx::ShareCredentials& credentials,
                  std::shared_ptr<void>& share)
{
    share.reset();

    if (credentials.user.empty()) {
        return true;
    }

    std::wstring name = shareOf(path);
    if (name.empty()) {
        return true;
    }

    auto connection = std::make_shared<ShareConnection>(std::move(name));
    if (!connection->connect(credentials)) {
        return false;
    }
    share = std::move(connection);
    return true;
}

} // namespace

bool Vhdx::isRemotePath(const std::string& path)
{
    return !shareOf(path).empty();
}

Vhdx::DiskReader::DiskReader() = default;
Vhdx::DiskReader::~DiskReader() = default;

bool Vhdx::DiskReader::open(const std::string& path, const std::string& changeId,
                            const ShareCredentials& credentials)
{
    m_impl.reset();
    m_properties = {};

    if (!checkElevated(path) || !VirtDisk::loadLibrary() || !connectShare(path, credentials, m_share)) {
        return false;
    }

    auto disk = std::make_unique<VirtualDisk>();
    if (!disk->open(path, true)) {
        return false;
    }

    disk->readProperties(m_properties);

    if (!changeId.empty() && !disk->collectChanges(changeId)) {
        return false;
    }
    if (!disk->attach()) {
        return false;
    }

    m_impl = std::move(disk);
    return true;
}

ABR_DISK_FORWARD_READER(Vhdx::DiskReader)

Vhdx::DiskWriter::DiskWriter() = default;
Vhdx::DiskWriter::~DiskWriter() = default;

bool Vhdx::DiskWriter::open(const std::string& path, bool keepExisting, const DiskGeometry& image,
                            const ShareCredentials& credentials)
{
    m_impl.reset();
    m_created = false;

    if (!checkElevated(path) || !VirtDisk::loadLibrary() || !connectShare(path, credentials, m_share)) {
        return false;
    }

    auto disk = std::make_unique<VirtualDisk>();
    bool isNew = false;

    const DWORD attributes = GetFileAttributesW(Win::widen(path).c_str());

    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();

        if (error != ERROR_FILE_NOT_FOUND) {
            std::cerr << "Error: failed to look up target '" << path << "': " << Win::errorText(error) << "\n";
            return false;
        }
        if (keepExisting) {
            std::cerr << "Error: target '" << path << "' does not exist; restore the full backup to it first\n";
            return false;
        }
        if (!disk->create(path, image)) {
            return false;
        }
        isNew = true;
    } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        std::cerr << "Error: target '" << path << "' is a directory\n";
        return false;
    } else if (!disk->open(path, false)) {
        return false;
    }

    if (!disk->attach() || !disk->claimForWriting()) {
        return false;
    }

    m_impl = std::move(disk);
    m_created = isNew;
    return true;
}

ABR_DISK_FORWARD_WRITER(Vhdx::DiskWriter)

std::unique_ptr<IDiskReader> Vhdx::openReader(const std::string& path, const std::string& changeId,
                                              DiskProperties& properties,
                                              const ShareCredentials& credentials)
{
    auto reader = std::make_unique<DiskReader>();
    if (!reader->open(path, changeId, credentials)) {
        return nullptr;
    }
    properties = reader->properties();
    return reader;
}

std::unique_ptr<IDiskWriter> Vhdx::openWriter(const std::string& path, bool keepExisting,
                                              const DiskGeometry& image, bool& created,
                                              const ShareCredentials& credentials)
{
    created = false;

    auto writer = std::make_unique<DiskWriter>();
    if (!writer->open(path, keepExisting, image, credentials)) {
        return nullptr;
    }
    created = writer->created();
    return writer;
}
