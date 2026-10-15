// Shared definitions for BBFS (Big Brother File System).
//
// The tutorial targets the FUSE 2.x API (API level 26). This must be defined
// before <fuse.h> is included anywhere.
#pragma once

#define FUSE_USE_VERSION 26

#include <cstdio>
#include <fuse.h>
#include <string>

// Private data handed to fuse_main(). FUSE gives it back to us inside every
// callback through fuse_get_context()->private_data.
struct bb_state {
    FILE *logfile;
    std::string rootdir; // canonical absolute path of the underlying directory
};

#define BB_DATA (static_cast<bb_state *>(fuse_get_context()->private_data))
