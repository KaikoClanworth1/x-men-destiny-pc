// Reads an Xbox 360 disc image (.iso): the game partition is an XDVDFS file system whose offset depends on the disc
// format (plain XISO, XGD2, XGD3). Lists its files, reads one, extracts all.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "util.h"

namespace xiso {

struct Entry {
    std::string path; // relative, backslashes
    u64 offset = 0;   // in the image file
    u64 size = 0;
    bool dir = false;
};

struct Image {
    fs::path file;
    u64 partition = 0;
    std::vector<Entry> entries;
    u64 total = 0; // bytes of all files
    const Entry* Find(const std::string& name) const; // case-insensitive, relative path
};

// False (with `error`) when the file is not an Xbox disc image.
bool Open(const fs::path& iso, Image& image, std::string& error);
// SHA-1 (hex) of one file inside the image.
std::string Sha1(const Image& image, const Entry& e);
// Writes every file under `out` (files already there with the right size are kept: an interrupted extract carries
// on). `skip`: top-level names left out. `progress(done bytes, total bytes)`.
bool Extract(const Image& image, const fs::path& out, const std::vector<std::string>& skip,
             const std::function<void(u64, u64)>& progress, std::atomic<bool>& cancel, std::string& error);

} // namespace xiso
