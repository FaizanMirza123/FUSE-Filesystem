#include "log.h"

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

FILE *log_open(const char *path) {
    FILE *logfile = std::fopen(path, "w");
    if (logfile == nullptr) {
        std::perror("logfile");
        std::exit(EXIT_FAILURE);
    }
    // Line buffering so `tail -F bbfs.log` shows operations as they happen.
    std::setvbuf(logfile, nullptr, _IOLBF, 0);
    return logfile;
}

void log_msg(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    std::vfprintf(BB_DATA->logfile, format, ap);
    va_end(ap);
}

// FUSE is multithreaded; hold the stream lock so multi-line dumps aren't
// interleaved with other threads' output.
namespace {
struct LogLock {
    FILE *f;
    LogLock() : f(BB_DATA->logfile) { flockfile(f); }
    ~LogLock() { funlockfile(f); }
};
} // namespace

int log_error(const char *func) {
    int err = errno;
    log_msg("    ERROR %s: %s\n", func, std::strerror(err));
    return -err;
}

int log_syscall(const char *func, int retstat, int min_ret) {
    int err = errno;
    log_msg("    %s returned %d\n", func, retstat);
    if (retstat < min_ret) {
        log_msg("    ERROR %s: %s\n", func, std::strerror(err));
        return -err;
    }
    return retstat;
}

void log_conn(const struct fuse_conn_info *conn) {
    LogLock lock;
    log_msg("    conn:\n");
    log_msg("        proto_major = %u\n", conn->proto_major);
    log_msg("        proto_minor = %u\n", conn->proto_minor);
    log_msg("        async_read = %u\n", conn->async_read);
    log_msg("        max_write = %u\n", conn->max_write);
    log_msg("        max_readahead = %u\n", conn->max_readahead);
    log_msg("        capable = 0x%08x\n", conn->capable);
    log_msg("        want = 0x%08x\n", conn->want);
    log_msg("        max_background = %u\n", conn->max_background);
    log_msg("        congestion_threshold = %u\n", conn->congestion_threshold);
}

void log_fi(const struct fuse_file_info *fi) {
    LogLock lock;
    log_msg("    fi:\n");
    log_msg("        flags = 0x%08x\n", fi->flags);
    log_msg("        fh_old = 0x%08lx\n", fi->fh_old);
    log_msg("        writepage = %d\n", fi->writepage);
    log_msg("        direct_io = %u\n", fi->direct_io);
    log_msg("        keep_cache = %u\n", fi->keep_cache);
    log_msg("        flush = %u\n", fi->flush);
    log_msg("        nonseekable = %u\n", fi->nonseekable);
    log_msg("        fh = 0x%016llx\n", static_cast<unsigned long long>(fi->fh));
    log_msg("        lock_owner = 0x%016llx\n", static_cast<unsigned long long>(fi->lock_owner));
}

void log_fuse_context(const struct fuse_context *context) {
    LogLock lock;
    log_msg("    context:\n");
    log_msg("        fuse = %p\n", static_cast<void *>(context->fuse));
    log_msg("        uid = %u\n", static_cast<unsigned>(context->uid));
    log_msg("        gid = %u\n", static_cast<unsigned>(context->gid));
    log_msg("        pid = %d\n", static_cast<int>(context->pid));
    log_msg("        private_data = %p\n", context->private_data);
    log_msg("        umask = %05o\n", static_cast<unsigned>(context->umask));
}

void log_stat(const struct stat *si) {
    LogLock lock;
    log_msg("    si:\n");
    log_msg("        st_dev = %lld\n", static_cast<long long>(si->st_dev));
    log_msg("        st_ino = %llu\n", static_cast<unsigned long long>(si->st_ino));
    log_msg("        st_mode = 0%o\n", static_cast<unsigned>(si->st_mode));
    log_msg("        st_nlink = %llu\n", static_cast<unsigned long long>(si->st_nlink));
    log_msg("        st_uid = %u\n", static_cast<unsigned>(si->st_uid));
    log_msg("        st_gid = %u\n", static_cast<unsigned>(si->st_gid));
    log_msg("        st_rdev = %lld\n", static_cast<long long>(si->st_rdev));
    log_msg("        st_size = %lld\n", static_cast<long long>(si->st_size));
    log_msg("        st_blksize = %lld\n", static_cast<long long>(si->st_blksize));
    log_msg("        st_blocks = %lld\n", static_cast<long long>(si->st_blocks));
    log_msg("        st_atime = 0x%08llx\n", static_cast<unsigned long long>(si->st_atime));
    log_msg("        st_mtime = 0x%08llx\n", static_cast<unsigned long long>(si->st_mtime));
    log_msg("        st_ctime = 0x%08llx\n", static_cast<unsigned long long>(si->st_ctime));
}

void log_statvfs(const struct statvfs *sv) {
    LogLock lock;
    log_msg("    sv:\n");
    log_msg("        f_bsize = %lu\n", static_cast<unsigned long>(sv->f_bsize));
    log_msg("        f_frsize = %lu\n", static_cast<unsigned long>(sv->f_frsize));
    log_msg("        f_blocks = %llu\n", static_cast<unsigned long long>(sv->f_blocks));
    log_msg("        f_bfree = %llu\n", static_cast<unsigned long long>(sv->f_bfree));
    log_msg("        f_bavail = %llu\n", static_cast<unsigned long long>(sv->f_bavail));
    log_msg("        f_files = %llu\n", static_cast<unsigned long long>(sv->f_files));
    log_msg("        f_ffree = %llu\n", static_cast<unsigned long long>(sv->f_ffree));
    log_msg("        f_favail = %llu\n", static_cast<unsigned long long>(sv->f_favail));
    log_msg("        f_fsid = %lu\n", static_cast<unsigned long>(sv->f_fsid));
    log_msg("        f_flag = 0x%08lx\n", static_cast<unsigned long>(sv->f_flag));
    log_msg("        f_namemax = %lu\n", static_cast<unsigned long>(sv->f_namemax));
}
