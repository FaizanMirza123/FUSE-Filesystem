// Logging helpers for BBFS. Everything is written to the log file stored in
// the filesystem's private data (see params.h).
#pragma once

#include "params.h"

#include <sys/stat.h>
#include <sys/statvfs.h>

FILE *log_open(const char *path);

void log_msg(const char *format, ...) __attribute__((format(printf, 1, 2)));

void log_conn(const struct fuse_conn_info *conn);
void log_fi(const struct fuse_file_info *fi);
void log_fuse_context(const struct fuse_context *context);
void log_stat(const struct stat *si);
void log_statvfs(const struct statvfs *sv);

// Log the current errno for `func` and return -errno (the FUSE error convention).
int log_error(const char *func);

// Log the result of a system call. If retstat < min_ret the call failed, so
// the error is logged and -errno is returned; otherwise retstat is returned.
int log_syscall(const char *func, int retstat, int min_ret);
