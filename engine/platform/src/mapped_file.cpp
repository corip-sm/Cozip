#include "cozip/platform/mapped_file.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cozip::platform
{
#ifdef _WIN32
namespace
{
using PrefetchVirtualMemoryFn = BOOL (WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);

std::string ToUtf8(const std::filesystem::path& path)
{
    return path.generic_string();
}

std::wstring ToWide(const std::filesystem::path& path)
{
    return path.wstring();
}

PrefetchVirtualMemoryFn ResolvePrefetchVirtualMemory() noexcept
{
    static const auto function = []() noexcept -> PrefetchVirtualMemoryFn {
        const auto module = GetModuleHandleW(L"kernel32.dll");
        if (module == nullptr)
        {
            return nullptr;
        }

        return reinterpret_cast<PrefetchVirtualMemoryFn>(
            GetProcAddress(module, "PrefetchVirtualMemory"));
    }();

    return function;
}

void PrefetchMappedRange(const MappedView& view) noexcept
{
    if (view.bytes.size() < (1u * 1024u * 1024u))
    {
        return;
    }

    auto* prefetch_virtual_memory = ResolvePrefetchVirtualMemory();
    if (prefetch_virtual_memory == nullptr)
    {
        return;
    }

    WIN32_MEMORY_RANGE_ENTRY range {};
    range.VirtualAddress = const_cast<std::byte*>(view.bytes.data());
    range.NumberOfBytes = view.bytes.size();
    prefetch_virtual_memory(GetCurrentProcess(), 1, &range, 0);
}
}

struct MappedFileReader::Impl
{
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle = nullptr;
    std::byte* mapped_base = nullptr;
    std::size_t mapped_size = 0;
    std::uint64_t mapped_offset = 0;
    std::size_t allocation_granularity = 64u * 1024u;
};

struct MappedFileWriter::Impl
{
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle = nullptr;
    std::byte* mapped_base = nullptr;
    std::size_t mapped_size = 0;
    std::uint64_t mapped_offset = 0;
    std::size_t allocation_granularity = 64u * 1024u;
};

MappedFileReader::MappedFileReader() = default;

MappedFileReader::~MappedFileReader()
{
    Close();
}

MappedFileReader::MappedFileReader(MappedFileReader&& other) noexcept = default;

MappedFileReader& MappedFileReader::operator=(MappedFileReader&& other) noexcept = default;

bool MappedFileReader::Open(const std::filesystem::path& path, std::string& error_message)
{
    Close();

    auto impl = std::make_unique<Impl>();
    SYSTEM_INFO system_info {};
    GetSystemInfo(&system_info);
    impl->allocation_granularity =
        std::max<std::size_t>(64u * 1024u, static_cast<std::size_t>(system_info.dwAllocationGranularity));

    const auto wide_path = ToWide(path);
    impl->file_handle = CreateFileW(
        wide_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (impl->file_handle == INVALID_HANDLE_VALUE)
    {
        error_message = "failed to open file mapping source: " + ToUtf8(path);
        return false;
    }

    LARGE_INTEGER file_size {};
    if (!GetFileSizeEx(impl->file_handle, &file_size) || file_size.QuadPart < 0)
    {
        error_message = "failed to read mapped file size: " + ToUtf8(path);
        CloseHandle(impl->file_handle);
        return false;
    }

    impl->mapping_handle = CreateFileMappingW(
        impl->file_handle,
        nullptr,
        PAGE_READONLY,
        0,
        0,
        nullptr);
    if (impl->mapping_handle == nullptr)
    {
        error_message = "failed to create file mapping: " + ToUtf8(path);
        CloseHandle(impl->file_handle);
        return false;
    }

    path_ = path;
    size_ = static_cast<std::uint64_t>(file_size.QuadPart);
    impl_ = std::move(impl);
    return true;
}

void MappedFileReader::Close() noexcept
{
    if (impl_)
    {
        if (impl_->mapped_base != nullptr)
        {
            UnmapViewOfFile(impl_->mapped_base);
            impl_->mapped_base = nullptr;
        }

        if (impl_->mapping_handle != nullptr)
        {
            CloseHandle(impl_->mapping_handle);
            impl_->mapping_handle = nullptr;
        }

        if (impl_->file_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(impl_->file_handle);
            impl_->file_handle = INVALID_HANDLE_VALUE;
        }
    }

    impl_.reset();
    path_.clear();
    size_ = 0;
    scratch_.clear();
}

bool MappedFileReader::IsOpen() const noexcept
{
    return !path_.empty();
}

std::uint64_t MappedFileReader::Size() const noexcept
{
    return size_;
}

bool MappedFileReader::MapWindow(std::uint64_t offset,
                                 std::size_t length,
                                 MappedView& view,
                                 std::string& error_message)
{
    if (!IsOpen() || impl_ == nullptr)
    {
        error_message = "mapped file is not open";
        return false;
    }

    if (offset > size_)
    {
        error_message = "offset is outside file";
        return false;
    }

    const auto clamped_length = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset));
    if (clamped_length == 0)
    {
        view.offset = offset;
        view.bytes = {};
        return true;
    }

    const auto granularity = static_cast<std::uint64_t>(impl_->allocation_granularity);
    const auto aligned_offset = offset - (offset % granularity);
    const auto intra_offset = static_cast<std::size_t>(offset - aligned_offset);
    const auto mapped_size = intra_offset + clamped_length;

    if (impl_->mapped_base != nullptr &&
        impl_->mapped_offset == aligned_offset &&
        impl_->mapped_size >= mapped_size)
    {
        view.offset = offset;
        view.bytes = std::span<const std::byte>(impl_->mapped_base + intra_offset, clamped_length);
        return true;
    }

    if (impl_->mapped_base != nullptr)
    {
        UnmapViewOfFile(impl_->mapped_base);
        impl_->mapped_base = nullptr;
        impl_->mapped_offset = 0;
        impl_->mapped_size = 0;
    }

    const DWORD offset_low = static_cast<DWORD>(aligned_offset & 0xFFFFFFFFull);
    const DWORD offset_high = static_cast<DWORD>((aligned_offset >> 32) & 0xFFFFFFFFull);
    void* mapped = MapViewOfFile(
        impl_->mapping_handle,
        FILE_MAP_READ,
        offset_high,
        offset_low,
        mapped_size);
    if (mapped == nullptr)
    {
        error_message = "failed to map file view: " + ToUtf8(path_);
        return false;
    }

    impl_->mapped_base = static_cast<std::byte*>(mapped);
    impl_->mapped_offset = aligned_offset;
    impl_->mapped_size = mapped_size;

    view.offset = offset;
    view.bytes = std::span<const std::byte>(impl_->mapped_base + intra_offset, clamped_length);
    PrefetchMappedRange(view);
    return true;
}

MappedFileWriter::MappedFileWriter() = default;

MappedFileWriter::~MappedFileWriter()
{
    Close();
}

MappedFileWriter::MappedFileWriter(MappedFileWriter&& other) noexcept = default;

MappedFileWriter& MappedFileWriter::operator=(MappedFileWriter&& other) noexcept = default;

bool MappedFileWriter::Open(const std::filesystem::path& path,
                            std::uint64_t size,
                            std::string& error_message)
{
    Close();

    auto impl = std::make_unique<Impl>();
    SYSTEM_INFO system_info {};
    GetSystemInfo(&system_info);
    impl->allocation_granularity =
        std::max<std::size_t>(64u * 1024u, static_cast<std::size_t>(system_info.dwAllocationGranularity));

    const auto wide_path = ToWide(path);
    impl->file_handle = CreateFileW(
        wide_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (impl->file_handle == INVALID_HANDLE_VALUE)
    {
        error_message = "failed to open mapped file target: " + ToUtf8(path);
        return false;
    }

    LARGE_INTEGER file_size {};
    file_size.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(impl->file_handle, file_size, nullptr, FILE_BEGIN) || !SetEndOfFile(impl->file_handle))
    {
        error_message = "failed to resize mapped file target: " + ToUtf8(path);
        CloseHandle(impl->file_handle);
        return false;
    }

    impl->mapping_handle = CreateFileMappingW(
        impl->file_handle,
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>((size >> 32) & 0xFFFFFFFFull),
        static_cast<DWORD>(size & 0xFFFFFFFFull),
        nullptr);
    if (impl->mapping_handle == nullptr && size > 0)
    {
        error_message = "failed to create writable file mapping: " + ToUtf8(path);
        CloseHandle(impl->file_handle);
        return false;
    }

    path_ = path;
    size_ = size;
    impl_ = std::move(impl);
    return true;
}

void MappedFileWriter::Close() noexcept
{
    if (impl_)
    {
        if (impl_->mapped_base != nullptr)
        {
            UnmapViewOfFile(impl_->mapped_base);
            impl_->mapped_base = nullptr;
        }

        if (impl_->mapping_handle != nullptr)
        {
            CloseHandle(impl_->mapping_handle);
            impl_->mapping_handle = nullptr;
        }

        if (impl_->file_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(impl_->file_handle);
            impl_->file_handle = INVALID_HANDLE_VALUE;
        }
    }

    impl_.reset();
    path_.clear();
    size_ = 0;
}

bool MappedFileWriter::IsOpen() const noexcept
{
    return !path_.empty();
}

std::uint64_t MappedFileWriter::Size() const noexcept
{
    return size_;
}

bool MappedFileWriter::MapWindow(std::uint64_t offset,
                                 std::size_t length,
                                 WritableMappedView& view,
                                 std::string& error_message)
{
    if (!IsOpen() || impl_ == nullptr)
    {
        error_message = "mapped file target is not open";
        return false;
    }

    if (offset > size_)
    {
        error_message = "offset is outside file";
        return false;
    }

    const auto clamped_length = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset));
    if (clamped_length == 0)
    {
        view.offset = offset;
        view.bytes = {};
        return true;
    }

    const auto granularity = static_cast<std::uint64_t>(impl_->allocation_granularity);
    const auto aligned_offset = offset - (offset % granularity);
    const auto intra_offset = static_cast<std::size_t>(offset - aligned_offset);
    const auto mapped_size = intra_offset + clamped_length;

    if (impl_->mapped_base != nullptr &&
        impl_->mapped_offset == aligned_offset &&
        impl_->mapped_size >= mapped_size)
    {
        view.offset = offset;
        view.bytes = std::span<std::byte>(impl_->mapped_base + intra_offset, clamped_length);
        return true;
    }

    if (impl_->mapped_base != nullptr)
    {
        UnmapViewOfFile(impl_->mapped_base);
        impl_->mapped_base = nullptr;
        impl_->mapped_offset = 0;
        impl_->mapped_size = 0;
    }

    const DWORD offset_low = static_cast<DWORD>(aligned_offset & 0xFFFFFFFFull);
    const DWORD offset_high = static_cast<DWORD>((aligned_offset >> 32) & 0xFFFFFFFFull);
    void* mapped = MapViewOfFile(
        impl_->mapping_handle,
        FILE_MAP_WRITE | FILE_MAP_READ,
        offset_high,
        offset_low,
        mapped_size);
    if (mapped == nullptr)
    {
        error_message = "failed to map writable file view: " + ToUtf8(path_);
        return false;
    }

    impl_->mapped_base = static_cast<std::byte*>(mapped);
    impl_->mapped_offset = aligned_offset;
    impl_->mapped_size = mapped_size;

    view.offset = offset;
    view.bytes = std::span<std::byte>(impl_->mapped_base + intra_offset, clamped_length);
    return true;
}
#else
namespace
{
std::string ToUtf8(const std::filesystem::path& path)
{
    return path.generic_string();
}

void PrefetchMappedRange(const MappedView& view) noexcept
{
    if (view.bytes.empty())
    {
        return;
    }

    (void)posix_madvise(
        const_cast<std::byte*>(view.bytes.data()),
        view.bytes.size(),
        POSIX_MADV_SEQUENTIAL);
}
}

struct MappedFileReader::Impl
{
    int fd = -1;
    std::byte* mapped_base = nullptr;
    std::size_t mapped_size = 0;
    std::uint64_t mapped_offset = 0;
    std::size_t allocation_granularity = 4096u;
};

struct MappedFileWriter::Impl
{
    int fd = -1;
    std::byte* mapped_base = nullptr;
    std::size_t mapped_size = 0;
    std::uint64_t mapped_offset = 0;
    std::size_t allocation_granularity = 4096u;
};

MappedFileReader::MappedFileReader() = default;

MappedFileReader::~MappedFileReader()
{
    Close();
}

MappedFileReader::MappedFileReader(MappedFileReader&& other) noexcept = default;

MappedFileReader& MappedFileReader::operator=(MappedFileReader&& other) noexcept = default;

bool MappedFileReader::Open(const std::filesystem::path& path, std::string& error_message)
{
    Close();

    auto impl = std::make_unique<Impl>();
    const long page_size = sysconf(_SC_PAGESIZE);
    impl->allocation_granularity =
        std::max<std::size_t>(4096u, page_size > 0 ? static_cast<std::size_t>(page_size) : 4096u);

    impl->fd = open(path.c_str(), O_RDONLY);
    if (impl->fd < 0)
    {
        error_message = "failed to open file mapping source: " + ToUtf8(path);
        return false;
    }

    struct stat st {};
    if (fstat(impl->fd, &st) != 0 || st.st_size < 0)
    {
        error_message = "failed to read mapped file size: " + ToUtf8(path);
        close(impl->fd);
        return false;
    }

    path_ = path;
    size_ = static_cast<std::uint64_t>(st.st_size);
    impl_ = std::move(impl);
    return true;
}

void MappedFileReader::Close() noexcept
{
    if (impl_)
    {
        if (impl_->mapped_base != nullptr)
        {
            munmap(impl_->mapped_base, impl_->mapped_size);
            impl_->mapped_base = nullptr;
        }

        if (impl_->fd >= 0)
        {
            close(impl_->fd);
            impl_->fd = -1;
        }
    }

    impl_.reset();
    path_.clear();
    size_ = 0;
    scratch_.clear();
}

bool MappedFileReader::IsOpen() const noexcept
{
    return !path_.empty();
}

std::uint64_t MappedFileReader::Size() const noexcept
{
    return size_;
}

bool MappedFileReader::MapWindow(std::uint64_t offset,
                                 std::size_t length,
                                 MappedView& view,
                                 std::string& error_message)
{
    if (!IsOpen() || impl_ == nullptr)
    {
        error_message = "mapped file is not open";
        return false;
    }

    if (offset > size_)
    {
        error_message = "offset is outside file";
        return false;
    }

    const auto clamped_length = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset));
    if (clamped_length == 0)
    {
        view.offset = offset;
        view.bytes = {};
        return true;
    }

    const auto granularity = static_cast<std::uint64_t>(impl_->allocation_granularity);
    const auto aligned_offset = offset - (offset % granularity);
    const auto intra_offset = static_cast<std::size_t>(offset - aligned_offset);
    const auto mapped_size = intra_offset + clamped_length;

    if (impl_->mapped_base != nullptr &&
        impl_->mapped_offset == aligned_offset &&
        impl_->mapped_size >= mapped_size)
    {
        view.offset = offset;
        view.bytes = std::span<const std::byte>(impl_->mapped_base + intra_offset, clamped_length);
        return true;
    }

    if (impl_->mapped_base != nullptr)
    {
        munmap(impl_->mapped_base, impl_->mapped_size);
        impl_->mapped_base = nullptr;
        impl_->mapped_offset = 0;
        impl_->mapped_size = 0;
    }

    void* mapped = mmap(
        nullptr,
        mapped_size,
        PROT_READ,
        MAP_PRIVATE,
        impl_->fd,
        static_cast<off_t>(aligned_offset));
    if (mapped == MAP_FAILED)
    {
        error_message = "failed to map file view: " + ToUtf8(path_);
        return false;
    }

    impl_->mapped_base = static_cast<std::byte*>(mapped);
    impl_->mapped_offset = aligned_offset;
    impl_->mapped_size = mapped_size;

    view.offset = offset;
    view.bytes = std::span<const std::byte>(impl_->mapped_base + intra_offset, clamped_length);
    PrefetchMappedRange(view);
    return true;
}

MappedFileWriter::MappedFileWriter() = default;

MappedFileWriter::~MappedFileWriter()
{
    Close();
}

MappedFileWriter::MappedFileWriter(MappedFileWriter&& other) noexcept = default;

MappedFileWriter& MappedFileWriter::operator=(MappedFileWriter&& other) noexcept = default;

bool MappedFileWriter::Open(const std::filesystem::path& path,
                            std::uint64_t size,
                            std::string& error_message)
{
    Close();

    auto impl = std::make_unique<Impl>();
    const long page_size = sysconf(_SC_PAGESIZE);
    impl->allocation_granularity =
        std::max<std::size_t>(4096u, page_size > 0 ? static_cast<std::size_t>(page_size) : 4096u);

    impl->fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (impl->fd < 0)
    {
        error_message = "failed to open mapped file target: " + ToUtf8(path);
        return false;
    }

    if (ftruncate(impl->fd, static_cast<off_t>(size)) != 0)
    {
        error_message = "failed to resize mapped file target: " + ToUtf8(path);
        close(impl->fd);
        return false;
    }

    path_ = path;
    size_ = size;
    impl_ = std::move(impl);
    return true;
}

void MappedFileWriter::Close() noexcept
{
    if (impl_)
    {
        if (impl_->mapped_base != nullptr)
        {
            munmap(impl_->mapped_base, impl_->mapped_size);
            impl_->mapped_base = nullptr;
        }

        if (impl_->fd >= 0)
        {
            close(impl_->fd);
            impl_->fd = -1;
        }
    }

    impl_.reset();
    path_.clear();
    size_ = 0;
}

bool MappedFileWriter::IsOpen() const noexcept
{
    return !path_.empty();
}

std::uint64_t MappedFileWriter::Size() const noexcept
{
    return size_;
}

bool MappedFileWriter::MapWindow(std::uint64_t offset,
                                 std::size_t length,
                                 WritableMappedView& view,
                                 std::string& error_message)
{
    if (!IsOpen() || impl_ == nullptr)
    {
        error_message = "mapped file target is not open";
        return false;
    }

    if (offset > size_)
    {
        error_message = "offset is outside file";
        return false;
    }

    const auto clamped_length = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset));
    if (clamped_length == 0)
    {
        view.offset = offset;
        view.bytes = {};
        return true;
    }

    const auto granularity = static_cast<std::uint64_t>(impl_->allocation_granularity);
    const auto aligned_offset = offset - (offset % granularity);
    const auto intra_offset = static_cast<std::size_t>(offset - aligned_offset);
    const auto mapped_size = intra_offset + clamped_length;

    if (impl_->mapped_base != nullptr &&
        impl_->mapped_offset == aligned_offset &&
        impl_->mapped_size >= mapped_size)
    {
        view.offset = offset;
        view.bytes = std::span<std::byte>(impl_->mapped_base + intra_offset, clamped_length);
        return true;
    }

    if (impl_->mapped_base != nullptr)
    {
        munmap(impl_->mapped_base, impl_->mapped_size);
        impl_->mapped_base = nullptr;
        impl_->mapped_offset = 0;
        impl_->mapped_size = 0;
    }

    void* mapped = mmap(
        nullptr,
        mapped_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        impl_->fd,
        static_cast<off_t>(aligned_offset));
    if (mapped == MAP_FAILED)
    {
        error_message = "failed to map writable file view: " + ToUtf8(path_);
        return false;
    }

    impl_->mapped_base = static_cast<std::byte*>(mapped);
    impl_->mapped_offset = aligned_offset;
    impl_->mapped_size = mapped_size;

    view.offset = offset;
    view.bytes = std::span<std::byte>(impl_->mapped_base + intra_offset, clamped_length);
    return true;
}
#endif
}
