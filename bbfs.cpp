// Big Brother File System (BBFS), based on "Writing a FUSE Filesystem: a
// Tutorial" by Joseph J. Pfeiffer, Jr.
// https://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/
//
// BBFS mirrors an existing directory (rootDir) at a mount point. Every file
// operation performed under the mount point is passed down to the matching
// file in rootDir, and logged to ./bbfs.log ("Big Brother is watching").
//
// Usage:  bbfs [FUSE and mount options] rootDir mountPoint
//
// Callback conventions (see the tutorial's "Callbacks" section):
//   - `path` is relative to the mount point (always starts with '/').
//   - Return 0 (or a byte count for read/write) on success, -errno on failure.

#include "log.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Translate a mount-relative path into the full path in the underlying
// directory. All BBFS operations go through this.
static int bb_fullpath(char fpath[PATH_MAX], const char *path) {
    const std::string &root = BB_DATA->rootdir;
    int n = std::snprintf(fpath, PATH_MAX, "%s%s", root.c_str(), path);
    log_msg("    bb_fullpath:  rootdir = \"%s\", path = \"%s\", fpath = \"%s\"\n",
            root.c_str(), path, fpath);
    if (n < 0 || n >= PATH_MAX) {
        log_msg("    ERROR bb_fullpath: path too long\n");
        return -ENAMETOOLONG;
    }
    return 0;
}

#define BB_FULLPATH(fpath, path)                                               \
    char fpath[PATH_MAX];                                                      \
    if (int rc_ = bb_fullpath(fpath, path)) return rc_

// ---------------------------------------------------------------------------
// File and directory metadata
// ---------------------------------------------------------------------------

// Get file attributes. Like stat(), but doesn't follow symlinks (FUSE does
// that itself through readlink).
static int bb_getattr(const char *path, struct stat *statbuf) {
    log_msg("\nbb_getattr(path=\"%s\", statbuf=%p)\n", path, static_cast<void *>(statbuf));
    BB_FULLPATH(fpath, path);

    int retstat = log_syscall("lstat", lstat(fpath, statbuf), 0);
    if (retstat == 0) log_stat(statbuf);
    return retstat;
}

// Read the target of a symlink. FUSE wants a null-terminated string (unlike
// the readlink() system call), and the target is passed through unchanged.
static int bb_readlink(const char *path, char *link, size_t size) {
    log_msg("\nbb_readlink(path=\"%s\", link=%p, size=%zu)\n", path, static_cast<void *>(link), size);
    BB_FULLPATH(fpath, path);
    if (size == 0) return -EINVAL;

    ssize_t len = readlink(fpath, link, size - 1);
    if (len < 0) return log_error("readlink");
    link[len] = '\0';
    log_msg("    link=\"%s\"\n", link);
    return 0;
}

// Create a non-directory, non-symlink node. Only called when create() isn't
// used (e.g. for FIFOs and device nodes).
static int bb_mknod(const char *path, mode_t mode, dev_t dev) {
    log_msg("\nbb_mknod(path=\"%s\", mode=0%3o, dev=%lld)\n", path,
            static_cast<unsigned>(mode), static_cast<long long>(dev));
    BB_FULLPATH(fpath, path);

    // On Linux mknod() could do all of this, but open()/mkfifo() are portable.
    if (S_ISREG(mode)) {
        int fd = log_syscall("open", open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode), 0);
        if (fd < 0) return fd;
        return log_syscall("close", close(fd), 0);
    }
    if (S_ISFIFO(mode)) return log_syscall("mkfifo", mkfifo(fpath, mode), 0);
    return log_syscall("mknod", mknod(fpath, mode, dev), 0);
}

static int bb_mkdir(const char *path, mode_t mode) {
    log_msg("\nbb_mkdir(path=\"%s\", mode=0%3o)\n", path, static_cast<unsigned>(mode));
    BB_FULLPATH(fpath, path);
    return log_syscall("mkdir", mkdir(fpath, mode), 0);
}

static int bb_unlink(const char *path) {
    log_msg("\nbb_unlink(path=\"%s\")\n", path);
    BB_FULLPATH(fpath, path);
    return log_syscall("unlink", unlink(fpath), 0);
}

static int bb_rmdir(const char *path) {
    log_msg("\nbb_rmdir(path=\"%s\")\n", path);
    BB_FULLPATH(fpath, path);
    return log_syscall("rmdir", rmdir(fpath), 0);
}

// Create a symbolic link. `path` is the link's *contents* and is stored
// as-is; only the location of the new link is translated.
static int bb_symlink(const char *path, const char *link) {
    log_msg("\nbb_symlink(path=\"%s\", link=\"%s\")\n", path, link);
    BB_FULLPATH(flink, link);
    return log_syscall("symlink", symlink(path, flink), 0);
}

// Both paths are relative to the mount point, so both are translated.
static int bb_rename(const char *path, const char *newpath) {
    log_msg("\nbb_rename(fpath=\"%s\", newpath=\"%s\")\n", path, newpath);
    BB_FULLPATH(fpath, path);
    BB_FULLPATH(fnewpath, newpath);
    return log_syscall("rename", rename(fpath, fnewpath), 0);
}

static int bb_link(const char *path, const char *newpath) {
    log_msg("\nbb_link(path=\"%s\", newpath=\"%s\")\n", path, newpath);
    BB_FULLPATH(fpath, path);
    BB_FULLPATH(fnewpath, newpath);
    return log_syscall("link", link(fpath, fnewpath), 0);
}

static int bb_chmod(const char *path, mode_t mode) {
    log_msg("\nbb_chmod(fpath=\"%s\", mode=0%03o)\n", path, static_cast<unsigned>(mode));
    BB_FULLPATH(fpath, path);
    return log_syscall("chmod", chmod(fpath, mode), 0);
}

static int bb_chown(const char *path, uid_t uid, gid_t gid) {
    log_msg("\nbb_chown(path=\"%s\", uid=%u, gid=%u)\n", path,
            static_cast<unsigned>(uid), static_cast<unsigned>(gid));
    BB_FULLPATH(fpath, path);
    return log_syscall("lchown", lchown(fpath, uid, gid), 0);
}

static int bb_truncate(const char *path, off_t newsize) {
    log_msg("\nbb_truncate(path=\"%s\", newsize=%lld)\n", path, static_cast<long long>(newsize));
    BB_FULLPATH(fpath, path);
    return log_syscall("truncate", truncate(fpath, newsize), 0);
}

// Change access/modification times with nanosecond precision.
static int bb_utimens(const char *path, const struct timespec tv[2]) {
    log_msg("\nbb_utimens(path=\"%s\", tv=%p)\n", path, static_cast<const void *>(tv));
    BB_FULLPATH(fpath, path);
    return log_syscall("utimensat", utimensat(AT_FDCWD, fpath, tv, AT_SYMLINK_NOFOLLOW), 0);
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

// Open a file. We open the underlying file and stash its descriptor in
// fi->fh so later read/write/release calls can use it. O_CREAT, O_EXCL and
// (by default) O_TRUNC are handled by FUSE before we get here.
static int bb_open(const char *path, struct fuse_file_info *fi) {
    log_msg("\nbb_open(path=\"%s\", fi=%p)\n", path, static_cast<void *>(fi));
    BB_FULLPATH(fpath, path);

    int fd = log_syscall("open", open(fpath, fi->flags), 0);
    if (fd < 0) return fd;

    fi->fh = static_cast<uint64_t>(fd);
    log_fi(fi);
    return 0;
}

// Create and open a file (called instead of open() when O_CREAT is set and
// the file doesn't exist yet).
static int bb_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    log_msg("\nbb_create(path=\"%s\", mode=0%03o, fi=%p)\n", path,
            static_cast<unsigned>(mode), static_cast<void *>(fi));
    BB_FULLPATH(fpath, path);

    int fd = log_syscall("open", open(fpath, fi->flags | O_CREAT, mode), 0);
    if (fd < 0) return fd;

    fi->fh = static_cast<uint64_t>(fd);
    log_fi(fi);
    return 0;
}

// Read from an open file. Returns the number of bytes read (FUSE passes that
// straight back to the caller), so this behaves like pread().
static int bb_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    log_msg("\nbb_read(path=\"%s\", buf=%p, size=%zu, offset=%lld, fi=%p)\n", path,
            static_cast<void *>(buf), size, static_cast<long long>(offset), static_cast<void *>(fi));
    log_fi(fi);
    return log_syscall("pread", static_cast<int>(pread(static_cast<int>(fi->fh), buf, size, offset)), 0);
}

static int bb_write(const char *path, const char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    log_msg("\nbb_write(path=\"%s\", buf=%p, size=%zu, offset=%lld, fi=%p)\n", path,
            static_cast<const void *>(buf), size, static_cast<long long>(offset),
            static_cast<void *>(fi));
    log_fi(fi);
    return log_syscall("pwrite", static_cast<int>(pwrite(static_cast<int>(fi->fh), buf, size, offset)), 0);
}

static int bb_statfs(const char *path, struct statvfs *statv) {
    log_msg("\nbb_statfs(path=\"%s\", statv=%p)\n", path, static_cast<void *>(statv));
    BB_FULLPATH(fpath, path);

    int retstat = log_syscall("statvfs", statvfs(fpath, statv), 0);
    if (retstat == 0) log_statvfs(statv);
    return retstat;
}

// Called on every close() of a file descriptor (possibly several times per
// open, e.g. after dup()). Nothing is buffered here, so there's nothing to do.
static int bb_flush(const char *path, struct fuse_file_info *fi) {
    log_msg("\nbb_flush(path=\"%s\", fi=%p)\n", path, static_cast<void *>(fi));
    log_fi(fi);
    return 0;
}

// Called once per open() when the last reference to the file is closed.
static int bb_release(const char *path, struct fuse_file_info *fi) {
    log_msg("\nbb_release(path=\"%s\", fi=%p)\n", path, static_cast<void *>(fi));
    log_fi(fi);
    return log_syscall("close", close(static_cast<int>(fi->fh)), 0);
}

// If datasync is nonzero, only the user data should be flushed, not metadata.
static int bb_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    log_msg("\nbb_fsync(path=\"%s\", datasync=%d, fi=%p)\n", path, datasync, static_cast<void *>(fi));
    log_fi(fi);
    int fd = static_cast<int>(fi->fh);
#ifdef __APPLE__
    // macOS has no fdatasync().
    return log_syscall("fsync", fsync(fd), 0);
#else
    if (datasync) return log_syscall("fdatasync", fdatasync(fd), 0);
    return log_syscall("fsync", fsync(fd), 0);
#endif
}

static int bb_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi) {
    log_msg("\nbb_ftruncate(path=\"%s\", offset=%lld, fi=%p)\n", path,
            static_cast<long long>(offset), static_cast<void *>(fi));
    log_fi(fi);
    return log_syscall("ftruncate", ftruncate(static_cast<int>(fi->fh), offset), 0);
}

// Like getattr, but on an open file (called e.g. right after create()).
static int bb_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi) {
    log_msg("\nbb_fgetattr(path=\"%s\", statbuf=%p, fi=%p)\n", path,
            static_cast<void *>(statbuf), static_cast<void *>(fi));
    log_fi(fi);

    // On the root directory fi->fh holds a DIR*, not a file descriptor, so
    // fall back to a plain getattr.
    if (std::strcmp(path, "/") == 0) return bb_getattr(path, statbuf);

    int retstat = log_syscall("fstat", fstat(static_cast<int>(fi->fh), statbuf), 0);
    if (retstat == 0) log_stat(statbuf);
    return retstat;
}

// ---------------------------------------------------------------------------
// Extended attributes. macFUSE / FUSE-T add a `position` argument, and the
// macOS xattr calls take XATTR_NOFOLLOW instead of having l* variants.
// ---------------------------------------------------------------------------

#ifdef __APPLE__
static int bb_setxattr(const char *path, const char *name, const char *value, size_t size,
                       int flags, uint32_t position) {
#else
static int bb_setxattr(const char *path, const char *name, const char *value, size_t size,
                       int flags) {
#endif
    log_msg("\nbb_setxattr(path=\"%s\", name=\"%s\", value=%p, size=%zu, flags=0x%08x)\n", path,
            name, static_cast<const void *>(value), size, flags);
    BB_FULLPATH(fpath, path);
#ifdef __APPLE__
    return log_syscall("setxattr",
                       setxattr(fpath, name, value, size, position, flags | XATTR_NOFOLLOW), 0);
#else
    return log_syscall("lsetxattr", lsetxattr(fpath, name, value, size, flags), 0);
#endif
}

#ifdef __APPLE__
static int bb_getxattr(const char *path, const char *name, char *value, size_t size,
                       uint32_t position) {
#else
static int bb_getxattr(const char *path, const char *name, char *value, size_t size) {
#endif
    log_msg("\nbb_getxattr(path=\"%s\", name=\"%s\", value=%p, size=%zu)\n", path, name,
            static_cast<void *>(value), size);
    BB_FULLPATH(fpath, path);
#ifdef __APPLE__
    ssize_t len = getxattr(fpath, name, value, size, position, XATTR_NOFOLLOW);
#else
    ssize_t len = lgetxattr(fpath, name, value, size);
#endif
    return log_syscall("getxattr", static_cast<int>(len), 0);
}

// Fills `list` with the null-separated attribute names. With size == 0 it
// just reports how much space is needed.
static int bb_listxattr(const char *path, char *list, size_t size) {
    log_msg("\nbb_listxattr(path=\"%s\", list=%p, size=%zu)\n", path, static_cast<void *>(list), size);
    BB_FULLPATH(fpath, path);
#ifdef __APPLE__
    ssize_t len = listxattr(fpath, list, size, XATTR_NOFOLLOW);
#else
    ssize_t len = llistxattr(fpath, list, size);
#endif
    int retstat = log_syscall("listxattr", static_cast<int>(len), 0);
    if (retstat > 0 && list != nullptr && size > 0) {
        log_msg("    returned attributes (length %d):\n", retstat);
        for (const char *p = list; p < list + retstat; p += std::strlen(p) + 1)
            log_msg("    \"%s\"\n", p);
    }
    return retstat;
}

static int bb_removexattr(const char *path, const char *name) {
    log_msg("\nbb_removexattr(path=\"%s\", name=\"%s\")\n", path, name);
    BB_FULLPATH(fpath, path);
#ifdef __APPLE__
    return log_syscall("removexattr", removexattr(fpath, name, XATTR_NOFOLLOW), 0);
#else
    return log_syscall("lremovexattr", lremovexattr(fpath, name), 0);
#endif
}

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

// Open a directory and keep the DIR* in fi->fh for readdir/releasedir.
static int bb_opendir(const char *path, struct fuse_file_info *fi) {
    log_msg("\nbb_opendir(path=\"%s\", fi=%p)\n", path, static_cast<void *>(fi));
    BB_FULLPATH(fpath, path);

    DIR *dp = opendir(fpath);
    log_msg("    opendir returned %p\n", static_cast<void *>(dp));
    if (dp == nullptr) return log_error("bb_opendir opendir");

    fi->fh = reinterpret_cast<uintptr_t>(dp);
    log_fi(fi);
    return 0;
}

// List a directory. We ignore `offset` and always pass 0 to filler(), which
// tells FUSE to read the whole directory in one go and manage offsets itself.
static int bb_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                      struct fuse_file_info *fi) {
    log_msg("\nbb_readdir(path=\"%s\", buf=%p, filler=%p, offset=%lld, fi=%p)\n", path, buf,
            reinterpret_cast<void *>(filler), static_cast<long long>(offset),
            static_cast<void *>(fi));

    DIR *dp = reinterpret_cast<DIR *>(static_cast<uintptr_t>(fi->fh));
    // A new listing on the same handle (e.g. after rewinddir()) must start over.
    rewinddir(dp);

    // Every directory has at least "." and "..", so an immediate NULL from
    // readdir() means an error.
    errno = 0;
    struct dirent *de = readdir(dp);
    if (de == nullptr) return log_error("bb_readdir readdir");

    // Copy the whole directory into the buffer. filler() returning non-zero
    // means the buffer is full.
    do {
        log_msg("    calling filler with name %s\n", de->d_name);
        if (filler(buf, de->d_name, nullptr, 0) != 0) {
            log_msg("    ERROR bb_readdir filler:  buffer full\n");
            return -ENOMEM;
        }
    } while ((de = readdir(dp)) != nullptr);

    log_fi(fi);
    return 0;
}

static int bb_releasedir(const char *path, struct fuse_file_info *fi) {
    log_msg("\nbb_releasedir(path=\"%s\", fi=%p)\n", path, static_cast<void *>(fi));
    log_fi(fi);
    closedir(reinterpret_cast<DIR *>(static_cast<uintptr_t>(fi->fh)));
    return 0;
}

// Directory contents are synced by the underlying filesystem; nothing to do.
static int bb_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
    log_msg("\nbb_fsyncdir(path=\"%s\", datasync=%d, fi=%p)\n", path, datasync,
            static_cast<void *>(fi));
    log_fi(fi);
    return 0;
}

// Check file access permissions (used for access() and chdir()).
static int bb_access(const char *path, int mask) {
    log_msg("\nbb_access(path=\"%s\", mask=0%o)\n", path, mask);
    BB_FULLPATH(fpath, path);
    return log_syscall("access", access(fpath, mask), 0);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Called once the filesystem is mounted. The return value becomes
// fuse_get_context()->private_data for every later callback, so hand back the
// bb_state that was passed to fuse_main().
static void *bb_init(struct fuse_conn_info *conn) {
    log_msg("\nbb_init()\n");
    log_conn(conn);
    log_fuse_context(fuse_get_context());
    return BB_DATA;
}

// Called on filesystem exit.
static void bb_destroy(void *userdata) {
    log_msg("\nbb_destroy(userdata=%p)\n", userdata);
}

// The callback table. Unset entries (e.g. lock, bmap) are unsupported.
static struct fuse_operations make_bb_oper() {
    struct fuse_operations op;
    std::memset(&op, 0, sizeof op);
    op.getattr = bb_getattr;
    op.readlink = bb_readlink;
    op.getdir = nullptr; // deprecated in favour of readdir
    op.mknod = bb_mknod;
    op.mkdir = bb_mkdir;
    op.unlink = bb_unlink;
    op.rmdir = bb_rmdir;
    op.symlink = bb_symlink;
    op.rename = bb_rename;
    op.link = bb_link;
    op.chmod = bb_chmod;
    op.chown = bb_chown;
    op.truncate = bb_truncate;
    op.utimens = bb_utimens;
    op.open = bb_open;
    op.read = bb_read;
    op.write = bb_write;
    op.statfs = bb_statfs;
    op.flush = bb_flush;
    op.release = bb_release;
    op.fsync = bb_fsync;
    op.setxattr = bb_setxattr;
    op.getxattr = bb_getxattr;
    op.listxattr = bb_listxattr;
    op.removexattr = bb_removexattr;
    op.opendir = bb_opendir;
    op.readdir = bb_readdir;
    op.releasedir = bb_releasedir;
    op.fsyncdir = bb_fsyncdir;
    op.init = bb_init;
    op.destroy = bb_destroy;
    op.access = bb_access;
    op.create = bb_create;
    op.ftruncate = bb_ftruncate;
    op.fgetattr = bb_fgetattr;
    return op;
}

// ---------------------------------------------------------------------------
// main: parse the command line and hand control to FUSE
// ---------------------------------------------------------------------------

[[noreturn]] static void bb_usage() {
    std::fprintf(stderr, "usage:  bbfs [FUSE and mount options] rootDir mountPoint\n");
    std::exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    // Running as root would let any user who can reach the mount act with
    // root's privileges on rootDir (see the tutorial's security section).
    if (getuid() == 0 || geteuid() == 0) {
        std::fprintf(stderr, "Running BBFS as root opens unacceptable security holes\n");
        return EXIT_FAILURE;
    }

    std::fprintf(stderr, "Fuse library version %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);

    // rootDir and mountPoint must be the last two arguments, and not options.
    if (argc < 3 || argv[argc - 2][0] == '-' || argv[argc - 1][0] == '-') bb_usage();

    auto *bb_data = new bb_state;

    // Canonicalize rootDir now: FUSE changes the working directory to "/"
    // when it daemonizes, which would break a relative path.
    char *root = realpath(argv[argc - 2], nullptr);
    if (root == nullptr) {
        std::perror(argv[argc - 2]);
        return EXIT_FAILURE;
    }
    bb_data->rootdir = root;
    std::free(root);

    struct stat st;
    if (stat(bb_data->rootdir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::fprintf(stderr, "%s: not a directory\n", bb_data->rootdir.c_str());
        return EXIT_FAILURE;
    }

    // Remove rootDir from argv so FUSE only sees its options and the mount point.
    argv[argc - 2] = argv[argc - 1];
    argv[argc - 1] = nullptr;
    argc--;

    bb_data->logfile = log_open("bbfs.log");

    static const struct fuse_operations bb_oper = make_bb_oper();

    std::fprintf(stderr, "about to call fuse_main\n");
    int fuse_stat = fuse_main(argc, argv, &bb_oper, bb_data);
    std::fprintf(stderr, "fuse_main returned %d\n", fuse_stat);

    return fuse_stat;
}
