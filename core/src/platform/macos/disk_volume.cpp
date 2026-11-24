#include "hm/disk_volume.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <sys/mount.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hm {
namespace {
std::string string_value(CFTypeRef value)
{
  if (!value || CFGetTypeID(value) != CFStringGetTypeID())
    return {};
  const auto *const str = static_cast<CFStringRef>(value);
  std::vector<char> bytes(
      static_cast<std::size_t>(CFStringGetMaximumSizeForEncoding(
          CFStringGetLength(str), kCFStringEncodingUTF8)) +
      1);
  return CFStringGetCString(str, bytes.data(),
                            static_cast<CFIndex>(bytes.size()),
                            kCFStringEncodingUTF8)
             ? bytes.data()
             : "";
}

std::optional<DiskVolume> external_volume(const struct statfs &fs,
                                          DASessionRef session)
{
  if ((fs.f_flags & MNT_LOCAL) == 0 || fs.f_mntonname[0] == '\0')
    return std::nullopt;
  DADiskRef disk =
      DADiskCreateFromBSDName(kCFAllocatorDefault, session, fs.f_mntfromname);
  if (!disk)
    return std::nullopt;
  CFDictionaryRef description = DADiskCopyDescription(disk);
  CFRelease(disk);
  if (!description)
    return std::nullopt;
  const auto *const internal =
      CFDictionaryGetValue(description, kDADiskDescriptionDeviceInternalKey);
  const auto protocol = string_value(
      CFDictionaryGetValue(description, kDADiskDescriptionDeviceProtocolKey));
  const auto *const uuid =
      CFDictionaryGetValue(description, kDADiskDescriptionVolumeUUIDKey);
  DiskVolume out;
  // Unknown device classification and disk images are not enrolled as external
  // drives.
  if (internal == kCFBooleanFalse && !protocol.empty() &&
      protocol != "Disk Image" && uuid &&
      CFGetTypeID(uuid) == CFUUIDGetTypeID()) {
    const auto *const text =
        CFUUIDCreateString(kCFAllocatorDefault, static_cast<CFUUIDRef>(uuid));
    out.id = string_value(text);
    if (text)
      CFRelease(text);
    out.name = string_value(
        CFDictionaryGetValue(description, kDADiskDescriptionVolumeNameKey));
    out.mount_path = fs.f_mntonname;
    if (out.name.empty())
      out.name = std::filesystem::path(out.mount_path).filename().string();
    out.session = std::to_string(fs.f_fsid.val[0]) + ":" +
                  std::to_string(fs.f_fsid.val[1]);
    out.total = fs.f_blocks * static_cast<std::uint64_t>(fs.f_bsize);
    out.free = fs.f_bavail * static_cast<std::uint64_t>(fs.f_bsize);
  }
  CFRelease(description);
  return out.id.empty() ? std::nullopt : std::optional(std::move(out));
}
} // namespace

Collected<std::vector<DiskVolume>> mounted_disk_volumes()
{
  const int count = getfsstat(nullptr, 0, MNT_NOWAIT);
  if (count < 0)
    return std::unexpected("Cannot enumerate mounted drives");
  std::vector<struct statfs> mounts(static_cast<std::size_t>(count) + 8);
  const int read = getfsstat(
      mounts.data(), static_cast<int>(mounts.size() * sizeof(struct statfs)),
      MNT_NOWAIT);
  if (read < 0 || static_cast<std::size_t>(read) >= mounts.size())
    return std::unexpected("Mounted drive list changed; retry discovery");
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  if (!session)
    return std::unexpected("Cannot inspect external drives");
  std::vector<DiskVolume> result;
  for (int index = 0; index < read; ++index) {
    const auto &mount = mounts[static_cast<std::size_t>(index)];
    if ((mount.f_flags & MNT_LOCAL) == 0)
      continue;
    struct statfs current{};
    if (statfs(mount.f_mntonname, &current) != 0)
      continue;
    if (auto volume = external_volume(current, session))
      result.push_back(std::move(*volume));
  }
  CFRelease(session);
  return result;
}

Collected<DiskVolume> disk_volume_for_path(const std::string &path)
{
  std::error_code error;
  const auto real = std::filesystem::canonical(path, error);
  if (error)
    return std::unexpected("Cannot access this folder. Connect or unlock the "
                           "drive and allow folder access.");
  struct statfs fs{};
  if (statfs(real.c_str(), &fs) != 0)
    return std::unexpected("Cannot identify this folder's drive");
  // macOS exposes the startup data filesystem separately from its sealed system
  // volume.
  if (std::string(fs.f_mntonname) == "/" ||
      std::string(fs.f_mntonname) == "/System/Volumes/Data") {
    DiskVolume internal;
    internal.name = "Internal disk";
    internal.mount_path = "/";
    return internal;
  }
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  if (!session)
    return std::unexpected("Cannot inspect this drive");
  auto volume = external_volume(fs, session);
  CFRelease(session);
  if (!volume)
    return std::unexpected(
        "Choose a local external drive with a filesystem UUID. Network shares "
        "and disk images are not supported.");
  return std::move(*volume);
}
} // namespace hm
