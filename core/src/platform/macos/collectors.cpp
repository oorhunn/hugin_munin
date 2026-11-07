#include "hm/collectors.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>

#include <cmath>
#include <memory>

namespace hm {
namespace {

struct CfRelease {
    void operator()(CFTypeRef object) const { CFRelease(object); }
};
using CfOwned = std::unique_ptr<const void, CfRelease>;

std::optional<std::int64_t> capacity(CFURLRef url, CFStringRef key) {
    CFTypeRef raw = nullptr;
    if (!CFURLCopyResourcePropertyForKey(url, key, &raw, nullptr) || raw == nullptr)
        return std::nullopt;
    const CfOwned value(raw);
    std::int64_t bytes = 0;
    if (CFGetTypeID(raw) != CFNumberGetTypeID() ||
        !CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberSInt64Type, &bytes))
        return std::nullopt;
    return bytes;
}

// Empty when the value is missing, not finite, or negative.
std::optional<double> number(CFTypeRef value) {
    double result = 0;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &result) ||
        !std::isfinite(result) || result < 0)
        return std::nullopt;
    return result;
}

struct GpuService {
    io_service_t service;
    std::optional<std::size_t> core_count;
};

// The accelerators and their core counts, found once; only the statistics change between reads.
const std::vector<GpuService> &gpu_services() {
    static const auto services = [] {
        std::vector<GpuService> result;
        io_iterator_t iterator = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"),
                                         &iterator) != KERN_SUCCESS)
            return result;
        while (const io_service_t service = IOIteratorNext(iterator)) {
            GpuService gpu{service, std::nullopt}; // keeps the iterator's reference
            const CfOwned cores(IORegistryEntryCreateCFProperty(service, CFSTR("gpu-core-count"),
                                                                kCFAllocatorDefault, 0));
            if (const auto count = number(cores.get()); count && *count > 0)
                gpu.core_count = static_cast<std::size_t>(*count);
            result.push_back(gpu);
        }
        IOObjectRelease(iterator);
        return result;
    }();
    return services;
}

} // namespace

Collected<std::vector<CpuCoreTicks>> collect_cpu_cores() {
    natural_t count = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t size = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &size) !=
        KERN_SUCCESS)
        return std::unexpected("Cannot read CPU core counters");
    // Release the Mach-allocated array on every exit path.
    struct Buffer {
        processor_info_array_t data;
        mach_msg_type_number_t size;
        ~Buffer() {
            vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(data),
                          size * sizeof(integer_t));
        }
    } buffer{info, size};
    if (size / PROCESSOR_CPU_LOAD_INFO_COUNT < count)
        return std::unexpected("Incomplete CPU core counters");
    std::vector<CpuCoreTicks> cores;
    cores.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        const auto ticks = reinterpret_cast<processor_cpu_load_info_t>(info)[i].cpu_ticks;
        cores.push_back({i, ticks[CPU_STATE_USER], ticks[CPU_STATE_SYSTEM], ticks[CPU_STATE_NICE],
                         ticks[CPU_STATE_IDLE]});
    }
    return cores;
}

Collected<Memory> collect_memory() {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                          &count) != KERN_SUCCESS)
        return std::unexpected("Cannot read virtual memory statistics");
    Memory memory;
    std::size_t size = sizeof(memory.total);
    if (sysctlbyname("hw.memsize", &memory.total, &size, nullptr, 0) != 0)
        return std::unexpected("Cannot read physical memory size");
    const std::uint64_t page = vm_kernel_page_size;
    // Purgeable pages can be reclaimed instantly, so they count as cache rather than app memory.
    memory.app = page * (vm.internal_page_count > vm.purgeable_count
                             ? vm.internal_page_count - vm.purgeable_count
                             : 0);
    memory.wired = page * vm.wire_count;
    memory.compressed = page * vm.compressor_page_count;
    memory.cached =
        page * (static_cast<std::uint64_t>(vm.external_page_count) + vm.purgeable_count);
    memory.used = memory.app + memory.wired + memory.compressed;
    xsw_usage swap{};
    size = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0) {
        memory.swap_used = swap.xsu_used;
        memory.swap_total = swap.xsu_total;
    }
    return memory;
}

Collected<std::vector<Gpu>> collect_gpus() {
    std::vector<Gpu> gpus;
    for (const auto &service : gpu_services()) {
        Gpu gpu{.utilization = std::nullopt, .core_count = service.core_count};
        const CfOwned statistics(IORegistryEntryCreateCFProperty(
            service.service, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0));
        if (statistics && CFGetTypeID(statistics.get()) == CFDictionaryGetTypeID()) {
            const auto dictionary = static_cast<CFDictionaryRef>(statistics.get());
            const auto utilization =
                number(CFDictionaryGetValue(dictionary, CFSTR("Device Utilization %")));
            if (utilization && *utilization <= 100)
                gpu.utilization = utilization;
        }
        gpus.push_back(gpu);
    }
    return gpus;
}

Collected<Disk> collect_disk() {
    const char root[] = "/";
    const CfOwned owned_url(CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(root), 1, true));
    if (!owned_url)
        return std::unexpected("Cannot create the startup volume URL");
    const auto url = static_cast<CFURLRef>(owned_url.get());
    const auto total = capacity(url, kCFURLVolumeTotalCapacityKey);
    const auto available = capacity(url, kCFURLVolumeAvailableCapacityForImportantUsageKey);
    if (!total || !available)
        return std::unexpected("Cannot read startup volume capacity");
    const auto disk = disk_from_capacity(*total, *available);
    if (!disk)
        return std::unexpected("Startup volume reports no capacity");
    return *disk;
}

} // namespace hm
