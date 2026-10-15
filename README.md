# FUSE-Filesystem

**BBFS — the Big Brother File System**, a FUSE filesystem written in C++,
based on Joseph J. Pfeiffer's
[Writing a FUSE Filesystem: a Tutorial](https://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/).

BBFS mirrors an existing directory (`rootDir`) at a mount point. Every
operation under the mount point is passed through to the real file in
`rootDir` and logged to `bbfs.log` in the directory you started it from
("Big Brother is watching").

## Files

| File | Purpose |
|------|---------|
| `bbfs.cpp` | The `bb_*` callbacks, the `fuse_operations` table and `main()` |
| `log.cpp` / `log.h` | `log_msg()` plus helpers that dump `stat`, `fuse_file_info`, etc. |
| `params.h` | `FUSE_USE_VERSION 26` and the private-data `struct bb_state` |
| `example/rootdir` | Sample directory to mount (`bogus.txt`) |
| `Dockerfile` | Linux build/test environment |

## Requirements

FUSE **2.x** development files (the tutorial uses API level 26):

- Debian/Ubuntu: `sudo apt install libfuse-dev fuse`
- Fedora: `sudo dnf install fuse-devel`
- macOS: [macFUSE](https://macfuse.github.io/) or [FUSE-T](https://www.fuse-t.org/)
  (`brew install --cask macfuse` / `brew install --cask fuse-t`)

## Build & run

```sh
make                       # builds ./bbfs
make mount                 # mounts example/rootdir on example/mountdir
ls -l example/mountdir     # same files as example/rootdir
tail -F example/bbfs.log   # watch operations get logged
make unmount               # fusermount -u (Linux) / umount (macOS)
```

Or by hand: `./bbfs [FUSE and mount options] rootDir mountPoint`
(e.g. add `-f` to stay in the foreground, `-d` for FUSE debug output).
BBFS refuses to run as root.

### Without FUSE installed (Docker)

```sh
docker build -t bbfs .
docker run --rm -it --device /dev/fuse --cap-add SYS_ADMIN bbfs
# inside the container:
make mount && ls example/mountdir
```

## How it works

- `main()` checks the arguments, turns `rootDir` into an absolute path with
  `realpath()`, removes it from `argv`, opens the log, and calls
  `fuse_main()` with the callback table and a `bb_state` as private data.
- Each callback logs its arguments, maps the path to the real one with
  `bb_fullpath()`, and makes the matching system call. It returns 0 (or a
  byte count) on success and `-errno` on failure.
- `open`/`create` save the real file descriptor in `fi->fh`, and `opendir`
  saves the `DIR*` there. `read`, `write`, `readdir`, `release`, and the
  other handle-based callbacks use that saved value.
- `readdir` passes offset 0 to `filler()`, so FUSE reads the whole
  directory in one call and tracks offsets itself.
