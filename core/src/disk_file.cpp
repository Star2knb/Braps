#include "rec/disk_file.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>

#include "rec/paths.h"

namespace rec {
namespace {

uint64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

double qpc_to_ms(uint64_t ticks) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(ticks) * 1000.0 / freq;
}

constexpr uint64_t kPreallocStep = 1ull << 30;  // 1 GiB (§11.2)

size_t round_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

}  // namespace

DiskFile::~DiskFile() {
    if (is_open()) {
        std::string ignored;
        close(&ignored);
    }
}

VOID CALLBACK DiskFile::on_complete(PVOID context, BOOLEAN) {
    static_cast<Slot*>(context)->done_qpc.store(qpc_now(), std::memory_order_release);
}

bool DiskFile::fail(const std::string& what, DWORD err) {
    if (error_.empty()) error_ = what + ": " + win32_error_text(err);
    return false;
}

bool DiskFile::open(const std::filesystem::path& path, size_t buffer_bytes, std::string* error) {
    path_ = path;
    buffer_bytes_ = round_up((std::max)(buffer_bytes, size_t(kSector)), kSector);
    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        *error = "can't create " + to_utf8(path.wstring()) + ": " + win32_error_text(GetLastError());
        return false;
    }
    patch_file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, nullptr);
    if (patch_file_ == INVALID_HANDLE_VALUE) {
        *error = "can't reopen " + to_utf8(path.wstring()) + ": " + win32_error_text(GetLastError());
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        return false;
    }
    // A file made in an NTFS-compressed folder is compressed too. Switch that off before the first byte.
    compression_removed_ = false;
    encrypted_ = false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_COMPRESSED)) {
        USHORT none = COMPRESSION_FORMAT_NONE;
        DWORD returned = 0;
        if (DeviceIoControl(patch_file_, FSCTL_SET_COMPRESSION, &none, sizeof(none), nullptr, 0, &returned, nullptr)) {
            const DWORD after = GetFileAttributesW(path.c_str());
            compression_removed_ = after != INVALID_FILE_ATTRIBUTES && !(after & FILE_ATTRIBUTE_COMPRESSED);
        }
    }
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_ENCRYPTED)) encrypted_ = true;
    for (Slot& s : slots_) {
        s.data = static_cast<uint8_t*>(VirtualAlloc(nullptr, buffer_bytes_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        s.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!s.data || !s.event) {
            *error = "out of memory for the write buffers";
            return false;
        }
    }
    cur_ = 0;
    fill_ = 0;
    base_ = position_ = allocated_ = 0;
    error_.clear();
    stats_ = Stats{};
    return true;
}

bool DiskFile::submit(Slot& s, uint32_t bytes) {
    // Keep the allocation ahead of the writes to limit fragmentation (§11.2). Failure is harmless.
    if (allocated_ < base_ + 4 * uint64_t(buffer_bytes_)) {
        allocated_ = base_ + kPreallocStep;
        FILE_ALLOCATION_INFO info;
        info.AllocationSize.QuadPart = LONGLONG(allocated_);
        SetFileInformationByHandle(file_, FileAllocationInfo, &info, sizeof(info));
    }
    s.file_offset = base_;
    s.bytes = bytes;
    std::memset(&s.ov, 0, sizeof(s.ov));
    s.ov.Offset = DWORD(base_ & 0xFFFFFFFFu);
    s.ov.OffsetHigh = DWORD(base_ >> 32);
    s.ov.hEvent = s.event;
    ResetEvent(s.event);
    s.done_qpc.store(0);
    if (!RegisterWaitForSingleObject(&s.wait, s.event, on_complete, &s, INFINITE, WT_EXECUTEONLYONCE)) return fail("write setup", GetLastError());
    s.submit_qpc = qpc_now();
    s.in_flight = true;
    if (!WriteFile(file_, s.data, bytes, nullptr, &s.ov) && GetLastError() != ERROR_IO_PENDING) {
        const DWORD err = GetLastError();
        UnregisterWaitEx(s.wait, nullptr);
        s.in_flight = false;
        return fail("write", err);
    }
    return true;
}

bool DiskFile::wait_for(Slot& s) {
    if (!s.in_flight) return true;
    WaitForSingleObject(s.event, INFINITE);
    UnregisterWaitEx(s.wait, INVALID_HANDLE_VALUE);  // waits for the completion callback, so done_qpc is set
    s.in_flight = false;
    DWORD got = 0;
    if (!GetOverlappedResult(file_, &s.ov, &got, FALSE)) return fail("write", GetLastError());
    if (got != s.bytes) return fail("short write", ERROR_WRITE_FAULT);
    const double ms = qpc_to_ms(s.done_qpc.load() - s.submit_qpc);
    ++stats_.writes;
    stats_.bytes_written += s.bytes;
    stats_.busy_seconds += ms / 1000.0;
    stats_.max_latency_ms = (std::max)(stats_.max_latency_ms, ms);
    if (ms > 0) stats_.peak_mb_s = (std::max)(stats_.peak_mb_s, double(s.bytes) / 1048576.0 / (ms / 1000.0));
    if (ms > slow_ms_) ++stats_.slow_writes;
    if (ms > very_slow_ms_) ++stats_.very_slow_writes;
    if (on_write) on_write(s.file_offset, s.bytes, ms);
    return true;
}

bool DiskFile::drain() {
    bool ok = true;
    for (Slot& s : slots_) ok = wait_for(s) && ok;
    return ok;
}

bool DiskFile::append(const void* data, size_t size) {
    if (!error_.empty()) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (size) {
        // Account for finished writes as soon as possible (their latency is exact either way).
        for (Slot& s : slots_)
            if (s.in_flight && s.done_qpc.load(std::memory_order_acquire) != 0 && !wait_for(s)) return false;

        Slot& cur = slots_[cur_];
        const size_t n = (std::min)(size, buffer_bytes_ - fill_);
        std::memcpy(cur.data + fill_, p, n);
        fill_ += n;
        p += n;
        size -= n;
        position_ += n;
        if (fill_ == buffer_bytes_) {
            if (!submit(cur, uint32_t(buffer_bytes_))) return false;
            base_ += buffer_bytes_;
            cur_ ^= 1;
            fill_ = 0;
            if (!wait_for(slots_[cur_])) return false;  // the buffer we are about to fill: its previous write
        }
    }
    return true;
}

bool DiskFile::patch(uint64_t offset, const void* data, size_t size) {
    if (!error_.empty()) return false;
    if (offset + size > position_) return false;  // refused; the stream itself is fine
    const uint8_t* p = static_cast<const uint8_t*>(data);

    // The part still in the current buffer.
    if (offset + size > base_) {
        const uint64_t from = (std::max)(offset, base_);
        std::memcpy(slots_[cur_].data + (from - base_), p + (from - offset), size_t(offset + size - from));
        if (offset >= base_) return true;
        size = size_t(base_ - offset);  // the rest lies before it
    }
    // The part already on its way to the disk or on it: read-modify-write whole sectors.
    if (!drain()) return false;
    const uint64_t start = offset & ~uint64_t(kSector - 1);
    const uint64_t stop = (offset + size + kSector - 1) & ~uint64_t(kSector - 1);
    const size_t bytes = size_t(stop - start);
    uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) return fail("patch buffer", ERROR_OUTOFMEMORY);
    bool ok = true;
    LARGE_INTEGER pos;
    pos.QuadPart = LONGLONG(start);
    DWORD got = 0;
    if (!SetFilePointerEx(patch_file_, pos, nullptr, FILE_BEGIN) || !ReadFile(patch_file_, buf, DWORD(bytes), &got, nullptr) || got != bytes) {
        ok = fail("patch read", GetLastError());
    } else {
        std::memcpy(buf + (offset - start), p, size);
        DWORD put = 0;
        if (!SetFilePointerEx(patch_file_, pos, nullptr, FILE_BEGIN) || !WriteFile(patch_file_, buf, DWORD(bytes), &put, nullptr) || put != bytes)
            ok = fail("patch write", GetLastError());
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return ok;
}

bool DiskFile::checkpoint() {
    if (!error_.empty()) return false;
    if (fill_ == 0) return drain();
    const uint32_t bytes = uint32_t(round_up(fill_, kSector));
    Slot& cur = slots_[cur_];
    std::memset(cur.data + fill_, 0, bytes - fill_);
    if (!wait_for(cur) || !submit(cur, bytes) || !wait_for(cur)) return false;
    return drain();
}

bool DiskFile::close(std::string* error) {
    if (!is_open()) return true;
    bool ok = error_.empty();
    if (ok && fill_ > 0) {
        const uint32_t bytes = uint32_t(round_up(fill_, kSector));
        Slot& cur = slots_[cur_];
        std::memset(cur.data + fill_, 0, bytes - fill_);
        ok = wait_for(cur) && submit(cur, bytes) && wait_for(cur);
    }
    ok = drain() && ok;
    if (ok) {
        FILE_END_OF_FILE_INFO eof;
        eof.EndOfFile.QuadPart = LONGLONG(position_);
        if (!SetFileInformationByHandle(file_, FileEndOfFileInfo, &eof, sizeof(eof))) ok = fail("truncate", GetLastError());
    }
    for (Slot& s : slots_) {
        if (s.in_flight) UnregisterWaitEx(s.wait, INVALID_HANDLE_VALUE);
        s.in_flight = false;
        if (s.event) CloseHandle(s.event);
        if (s.data) VirtualFree(s.data, 0, MEM_RELEASE);
        s.event = nullptr;
        s.data = nullptr;
    }
    CloseHandle(file_);
    CloseHandle(patch_file_);
    file_ = patch_file_ = INVALID_HANDLE_VALUE;
    if (!ok && error) *error = error_;
    return ok;
}

}  // namespace rec
