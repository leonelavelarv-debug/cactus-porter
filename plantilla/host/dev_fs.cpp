// NAND por juego. Cada componente se abre relativo al directorio ya validado.
#include "dev_fs.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ios {
namespace {
constexpr s32 invalid = -101, denied = -102, exists = -105, missing = -106;
constexpr s32 io_error = -114;
s32 error() {
    switch (errno) {
    case EEXIST: case ENOTEMPTY: return exists;
    case ENOENT: case ENOTDIR: return missing;
    case EACCES: case EPERM: case ELOOP: return denied;
    case ENOSPC: return -108;
    default: return io_error;
    }
}

struct Parent {
    int fd = -1;
    std::string name;
    ~Parent() { if (fd >= 0) ::close(fd); }
    Parent() = default;
    Parent(const Parent&) = delete;
    Parent& operator=(const Parent&) = delete;
};

std::string path_arg(const u8* data, u32 length) {
    if (!data || !length) return {};
    const auto* end = static_cast<const u8*>(memchr(data, 0, std::min(length, 64u)));
    if (!end) return {};
    return {reinterpret_cast<const char*>(data), size_t(end - data)};
}

struct Usage { uint64_t blocks = 0, inodes = 0; };

// Consume el descriptor incluso cuando falla la enumeración.
s32 usage_dir(int fd, Usage& usage, unsigned depth) {
    DIR* dir = fdopendir(fd);
    if (!dir) { auto rc = error(); ::close(fd); return rc; }
    ++usage.inodes;
    s32 rc = IPC_OK;
    for (;;) {
        errno = 0;
        auto* entry = readdir(dir);
        if (!entry) { if (errno) rc = error(); break; }
        const char* name = entry->d_name;
        if (!strcmp(name,".") || !strcmp(name,"..")) continue;
        struct stat st{};
        if (fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)) { rc = error(); break; }
        if (S_ISREG(st.st_mode)) {
            ++usage.inodes;
            usage.blocks += (uint64_t(st.st_size) + 16383) / 16384;
        } else if (S_ISDIR(st.st_mode)) {
            if (depth >= 64) { rc = invalid; break; }
            int child = openat(fd,name,O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { rc = error(); break; }
            rc = usage_dir(child,usage,depth+1);
            if (rc < 0) break;
        }
    }
    closedir(dir);
    return rc;
}

class DevFS final : public Device {
 public:
    explicit DevFS(int root) : root_(root) {}
    ~DevFS() override { ::close(root_); }
    s32 open(const char* path, u32 mode) override {
        if (!strcmp(path, "/dev/fs")) return 0;
        if (mode < 1 || mode > 3) return invalid;
        Parent p;
        s32 rc = parent(path, p);
        if (rc < 0) return rc;
        const int access[] = {0, O_RDONLY, O_WRONLY, O_RDWR};
        int fd = openat(p.fd, p.name.c_str(), access[mode] | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) return error();
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
            ::close(fd);
            return denied;
        }
        return fd + 1;
    }
    s32 close(s32 h) override { return h && ::close(h-1) ? error() : IPC_OK; }
    s32 read(s32 h, u8* out, u32 n) override {
        if (!h || n > INT_MAX) return invalid;
        const auto result = ::read(h-1, out, n);
        return result < 0 ? error() : s32(result);
    }
    s32 write(s32 h, const u8* in, u32 n) override {
        if (!h || n > INT_MAX) return invalid;
        const auto result = ::write(h-1, in, n);
        return result < 0 ? error() : s32(result);
    }
    s32 seek(s32 h, s32 off, u32 whence) override {
        if (!h || whence > 2) return invalid;
        auto pos = lseek(h-1, 0, SEEK_CUR);
        struct stat st{};
        if (pos < 0 || fstat(h-1, &st)) return error();
        int64_t target = whence == 0 ? 0 : whence == 1 ? pos : st.st_size;
        if (target > INT64_MAX - std::max(off, 0)) return invalid;
        target += off;
        if (target < 0 || target > INT_MAX || target > st.st_size) return invalid;
        return lseek(h-1, target, SEEK_SET) < 0 ? error() : s32(target);
    }
    s32 ioctl(s32 h, u32 req, u8* in, u32 n, u8* out, u32 m) override {
        if (h) {
            if (req != IOCTL_FS_GET_FILE_STATS || !out || m < 8) return invalid;
            struct stat st{};
            const auto pos = lseek(h-1, 0, SEEK_CUR);
            if (pos < 0 || fstat(h-1, &st)) return error();
            if (st.st_size > UINT32_MAX || pos > UINT32_MAX) return invalid;
            wbe32(out, u32(st.st_size)); wbe32(out+4, u32(pos));
            return IPC_OK;
        }
        if (req == IOCTL_FS_SHUTDOWN) return IPC_OK;
        if (req == IOCTL_FS_CREATE || req == IOCTL_FS_CREATE_DIR) {
            if (!in || n < 74) return invalid;
            return mutate(req, path_arg(in+6, 64), {});
        }
        if (req == IOCTL_FS_GET_ATTR) {
            auto path = path_arg(in,n);
            struct stat st{};
            auto rc = attributes(path,st);
            if (rc < 0) return rc;
            if (!out || m < 74) return invalid;
            memset(out,0,74);
            memcpy(out+6,path.data(),path.size());
            out[70] = 3; // Perfil local de un solo usuario, lectura y escritura.
            return IPC_OK;
        }
        if (req == IOCTL_FS_DELETE) return mutate(req, path_arg(in,n), {});
        if (req == IOCTL_FS_RENAME && in && n >= 128)
            return mutate(req, path_arg(in,64), path_arg(in+64,64));
        return invalid;
    }
    s32 ioctlv(s32 h, u32 req, u32 nin, u32 nout, IOVec* v) override {
        if (h) return invalid;
        if (req == IOCTL_FS_GET_ATTR && v && nin == 1 && nout == 1) {
            if (!v[1].data || v[1].len < 8) return invalid;
            struct stat st{};
            auto rc = attributes(path_arg(v[0].data,v[0].len),st);
            if (rc < 0) return rc;
            wbe32(v[1].data,S_ISDIR(st.st_mode) ? 0x10 : 0);
            wbe32(v[1].data+4,S_ISREG(st.st_mode) ? u32(st.st_size) : 0);
            return IPC_OK;
        }
        if (req == IOCTL_FS_GET_USAGE && v && nin == 1 && nout == 2) {
            if (!v[1].data || !v[2].data || v[1].len < 4 || v[2].len < 4) return invalid;
            auto path = path_arg(v[0].data,v[0].len);
            Parent p;
            int fd;
            if (path == "/") fd = openat(root_,".",O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            else {
                auto rc = parent(path,p);
                if (rc < 0) return rc;
                fd = openat(p.fd,p.name.c_str(),O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            }
            if (fd < 0) return error();
            Usage usage;
            auto rc = usage_dir(fd,usage,0);
            if (rc < 0) return rc;
            if (usage.blocks > UINT32_MAX || usage.inodes > UINT32_MAX) return invalid;
            wbe32(v[1].data,u32(usage.blocks)); wbe32(v[2].data,u32(usage.inodes));
            return IPC_OK;
        }
        if (nout) return invalid;
        if (req == IOCTL_FS_SHUTDOWN && nin == 0) return IPC_OK;
        if (!v || nin != (req == IOCTL_FS_RENAME ? 2u : 1u)) return invalid;
        return mutate(req, path_arg(v[0].data,v[0].len),
                      nin == 2 ? path_arg(v[1].data,v[1].len) : std::string{});
    }
 private:
    int root_;
    s32 attributes(const std::string& path, struct stat& st) const {
        if (path == "/") return fstat(root_,&st) ? error() : IPC_OK;
        Parent p;
        auto rc = parent(path,p);
        if (rc < 0) return rc;
        if (fstatat(p.fd,p.name.c_str(),&st,AT_SYMLINK_NOFOLLOW)) return error();
        return S_ISDIR(st.st_mode) || S_ISREG(st.st_mode) ? IPC_OK : denied;
    }
    s32 parent(const std::string& path, Parent& p) const {
        if (path.empty() || path[0] != '/' || path.size() >= 64) return invalid;
        p.fd = dup(root_);
        if (p.fd < 0) return error();
        size_t start = 1;
        for (;;) {
            auto slash = path.find('/', start);
            p.name = path.substr(start, slash-start);
            if (p.name.empty() || p.name == "." || p.name == "..") return invalid;
            if (slash == std::string::npos) return IPC_OK;
            int child = openat(p.fd, p.name.c_str(), O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) return error();
            ::close(p.fd);
            p.fd = child;
            start = slash+1;
        }
    }
    s32 mutate(u32 req, const std::string& path, const std::string& dst) {
        Parent p;
        s32 rc = parent(path, p);
        if (rc < 0) return rc;
        const char* name = p.name.c_str();
        switch (req) {
        case IOCTL_FS_CREATE: {
            int fd = openat(p.fd, name, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0) return error();
            return ::close(fd) ? error() : IPC_OK;
        }
        case IOCTL_FS_CREATE_DIR: return mkdirat(p.fd,name,0700) ? error() : IPC_OK;
        case IOCTL_FS_DELETE:
            if (!unlinkat(p.fd,name,0)) return IPC_OK;
            if (errno != EISDIR) return error();
            return unlinkat(p.fd,name,AT_REMOVEDIR) ? error() : IPC_OK;
        case IOCTL_FS_RENAME: {
            Parent dest;
            rc = parent(dst, dest);
            if (rc < 0) return rc;
            return renameat(p.fd,name,dest.fd,dest.name.c_str()) ? error() : IPC_OK;
        }
        default: return invalid;
        }
    }
};
}

std::shared_ptr<Device> make_dev_fs(const std::string& root) {
    if (mkdir(root.c_str(),0700) && errno != EEXIST) return {};
    int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return {};
    return std::make_shared<DevFS>(fd);
}
}
