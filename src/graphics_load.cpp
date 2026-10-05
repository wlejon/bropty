// Transmission media, decompression and pixel formats (see graphics_load.h).
#include "graphics_load.h"

#include "bropty/terminal.h"
#include "inflate.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bropty::detail {

namespace fs = std::filesystem;

namespace {

fs::path utf8_path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string generic_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// How many bytes to read: S (or the rest of the object after O).
bool span_of(uint64_t object_size, const KittyCommand& c, size_t cap, uint64_t& n, std::string& error) {
    if (c.offset > object_size) {
        error = "ENODATA:Offset is past the end of the data";
        return false;
    }
    n = c.size ? std::min<uint64_t>(c.size, object_size - c.offset) : object_size - c.offset;
    if (n > cap) {
        error = "EFBIG:Transmission is too large";
        return false;
    }
    return true;
}

#if !defined(_WIN32)
bool sensitive(const std::string& real) {
    for (const char* p : {"/proc/", "/sys/", "/dev/"})
        if (real.rfind(p, 0) == 0) return true;
    return false;
}
#endif

bool read_file(const std::string& path, const KittyCommand& c, size_t cap, std::vector<uint8_t>& out,
               std::string& error) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        error = "EINVAL:Invalid file path";
        return false;
    }
#if defined(_WIN32)
    std::error_code ec;
    const fs::path p = utf8_path(path);
    if (!fs::is_regular_file(p, ec)) {  // follows symlinks; devices and directories are refused
        error = "EBADF:Not a regular file";
        return false;
    }
    const uint64_t size = fs::file_size(p, ec);
    if (ec) {
        error = "EBADF:Failed to read file";
        return false;
    }
    uint64_t n;
    if (!span_of(size, c, cap, n, error)) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        error = "EBADF:Failed to open file";
        return false;
    }
    f.seekg(std::streamoff(c.offset));
    out.resize(size_t(n));
    if (n && !f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n))) {
        error = "EBADF:Failed to read file";
        return false;
    }
    return true;
#else
    char* real = ::realpath(path.c_str(), nullptr);
    if (!real) {
        error = "EBADF:Failed to resolve file path";
        return false;
    }
    const std::string resolved(real);
    std::free(real);
    if (sensitive(resolved)) {
        error = "EPERM:Refusing to read from a system directory";
        return false;
    }
    const int fd = ::open(resolved.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        error = "EBADF:Failed to open file";
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        error = "EBADF:Not a regular file";
        return false;
    }
    uint64_t n;
    if (!span_of(uint64_t(st.st_size), c, cap, n, error)) {
        ::close(fd);
        return false;
    }
    out.resize(size_t(n));
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::pread(fd, out.data() + got, size_t(n) - got, off_t(c.offset + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += size_t(r);
    }
    ::close(fd);
    if (got != n) {
        error = "EBADF:Failed to read file";
        return false;
    }
    return true;
#endif
}

bool read_shared_memory(const std::string& name, const KittyCommand& c, size_t cap, std::vector<uint8_t>& out,
                        std::string& error) {
    if (name.empty() || name.find('\0') != std::string::npos) {
        error = "EINVAL:Invalid shared memory name";
        return false;
    }
#if defined(_WIN32)
    const int wn = MultiByteToWideChar(CP_UTF8, 0, name.data(), int(name.size()), nullptr, 0);
    std::wstring wname(size_t(std::max(wn, 0)), L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, name.data(), int(name.size()), wname.data(), wn);
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, wname.c_str());
    if (!h) {
        error = "EBADF:Failed to open shared memory";
        return false;
    }
    const void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(h);
        error = "EBADF:Failed to map shared memory";
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    const uint64_t size = VirtualQuery(view, &mbi, sizeof mbi) ? uint64_t(mbi.RegionSize) : 0;
    uint64_t n = 0;
    const bool ok = span_of(size, c, cap, n, error);
    if (ok) out.assign(static_cast<const uint8_t*>(view) + c.offset, static_cast<const uint8_t*>(view) + c.offset + n);
    UnmapViewOfFile(view);
    CloseHandle(h);
    return ok;
#else
    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        error = "EBADF:Failed to open shared memory";
        return false;
    }
    struct stat st {};
    bool ok = ::fstat(fd, &st) == 0;
    uint64_t n = 0;
    if (ok) ok = span_of(uint64_t(st.st_size), c, cap, n, error);
    else error = "EBADF:Failed to stat shared memory";
    if (ok && n) {
        void* m = ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            ok = false;
            error = "EBADF:Failed to map shared memory";
        } else {
            const auto* b = static_cast<const uint8_t*>(m) + c.offset;
            out.assign(b, b + n);
            ::munmap(m, size_t(st.st_size));
        }
    } else if (ok) {
        out.clear();
    }
    ::close(fd);
    ::shm_unlink(name.c_str());  // the terminal owns the object once it is sent
    return ok;
#endif
}

} // namespace

bool acceptable_temp_file(const std::string& path) {
    if (path.find("tty-graphics-protocol") == std::string::npos) return false;
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(utf8_path(path), ec);
    if (ec) return false;
    std::vector<fs::path> dirs;
#if !defined(_WIN32)
    dirs.emplace_back("/tmp");
    dirs.emplace_back("/dev/shm");
#endif
#if !defined(_WIN32)  // Windows: temp_directory_path() already reads TMP / TEMP
    if (const char* t = std::getenv("TMPDIR"); t && *t) dirs.push_back(utf8_path(t));
#endif
    const fs::path sys = fs::temp_directory_path(ec);
    if (!ec) dirs.push_back(sys);
    const std::string file = generic_utf8(real);
    for (const fs::path& d : dirs) {
        std::error_code e2;
        const fs::path rd = fs::weakly_canonical(d, e2);
        if (e2) continue;
        std::string dir = generic_utf8(rd);
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        if (file.size() > dir.size() + 1 && file.compare(0, dir.size(), dir) == 0 && file[dir.size()] == '/')
            return true;
    }
    return false;
}

bool read_medium(const KittyCommand& c, std::vector<uint8_t>& data, const GraphicsOptions& o, std::string& error) {
    if (c.medium == 'd') return true;
    const std::string name(data.begin(), data.end());
    std::vector<uint8_t> out;
    switch (c.medium) {
    case 'f':
        if (!o.allow_files) {
            error = "EPERM:File transmission is disabled";
            return false;
        }
        if (!read_file(name, c, o.max_transmission_bytes, out, error)) return false;
        break;
    case 't': {
        if (!o.allow_temp_files) {
            error = "EPERM:Temporary file transmission is disabled";
            return false;
        }
        if (!acceptable_temp_file(name)) {
            error = "EPERM:Not a temporary file in a temporary directory";
            return false;
        }
        const bool ok = read_file(name, c, o.max_transmission_bytes, out, error);
        std::error_code ec;
        fs::remove(utf8_path(name), ec);
        if (!ok) return false;
        break;
    }
    case 's':
        if (!o.allow_shared_memory) {
            error = "EPERM:Shared memory transmission is disabled";
            return false;
        }
        if (!read_shared_memory(name, c, o.max_transmission_bytes, out, error)) return false;
        break;
    default:
        error = "EINVAL:Unknown transmission medium";
        return false;
    }
    data = std::move(out);
    return true;
}

bool decode_with_host(std::string_view data, const GraphicsOptions& o, TerminalHost* host, DecodedImage& out,
                      std::string& error) {
    out = DecodedImage{};
    const ImageLimits limits{o.max_width, o.max_height, o.storage_limit};
    if (!host || !host->decode_image(data, limits, out)) {
        error = "EBADPNG:Image could not be decoded";
        return false;
    }
    if (out.width == 0 || out.height == 0 || out.frames.empty() || out.width > o.max_width ||
        out.height > o.max_height) {
        error = "EINVAL:Decoded image has an invalid size";
        return false;
    }
    const size_t frame = size_t(out.width) * out.height * 4;
    if (frame * out.frames.size() > o.storage_limit) {
        error = "ENOSPC:Image is larger than the storage quota";
        return false;
    }
    for (const DecodedFrame& f : out.frames) {
        if (f.rgba.size() != frame) {
            error = "EINVAL:Decoded frame has the wrong size";
            return false;
        }
    }
    return true;
}

bool decode_kitty_pixels(const KittyCommand& c, std::vector<uint8_t>& data, const GraphicsOptions& o,
                         TerminalHost* host, DecodedImage& out, std::string& error) {
    out = DecodedImage{};
    const bool raw = c.format == 24 || c.format == 32;
    if (!raw && c.format != 100) {
        error = "EINVAL:Unknown image format";
        return false;
    }
    const uint32_t bpp = c.format == 24 ? 3 : 4;
    if (raw) {
        if (c.width == 0 || c.height == 0) {
            error = "EINVAL:Zero width or height";
            return false;
        }
        if (c.width > o.max_width || c.height > o.max_height) {
            error = "EFBIG:Image dimensions are too large";
            return false;
        }
    }
    const size_t expected = raw ? size_t(c.width) * c.height * bpp : 0;
    if (raw && expected > o.storage_limit) {
        error = "ENOSPC:Image is larger than the storage quota";
        return false;
    }
    if (c.compression == 'z') {
        std::vector<uint8_t> inflated;
        const size_t cap = raw ? expected : (c.size ? size_t(c.size) : o.max_transmission_bytes);
        const InflateStatus s = zlib_inflate(
            std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), inflated, cap);
        // A stream that runs past the image is fine: the image is complete.
        const bool full = s == InflateStatus::TooLarge && raw && inflated.size() == expected;
        if (s != InflateStatus::Ok && !full) {
            error = std::string("EINVAL:") + inflate_status_name(s);
            return false;
        }
        data = std::move(inflated);
    } else if (c.compression != 0) {
        error = "EINVAL:Unknown compression";
        return false;
    }
    if (!raw) {
        return decode_with_host(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), o, host,
                                out, error);
    }
    if (data.size() < expected) {
        error = "ENODATA:Insufficient image data";
        return false;
    }
    out.width = c.width;
    out.height = c.height;
    out.frames.resize(1);
    std::vector<uint8_t>& px = out.frames[0].rgba;
    if (bpp == 4) {
        data.resize(expected);
        px = std::move(data);
    } else {
        px.resize(size_t(c.width) * c.height * 4);
        const uint8_t* s = data.data();
        uint8_t* d = px.data();
        for (size_t i = 0, n = size_t(c.width) * c.height; i < n; ++i, s += 3, d += 4) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
    return true;
}

} // namespace bropty::detail
