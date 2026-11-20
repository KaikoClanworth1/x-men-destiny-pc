#include "xiso.h"

#include <cstring>
#include <fstream>

#include <windows.h>
#include <bcrypt.h>

namespace xiso {

namespace {

constexpr u64 kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// Partition offsets: plain XISO, XGD2, XGD3, XGD1.
constexpr u64 kOffsets[] = {0, 0xFD90000, 0x2080000, 0x18300000};

u32 Le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u16 Le16(const u8* p) { return u16(p[0] | p[1] << 8); }

bool ReadAt(std::ifstream& f, u64 offset, void* dst, size_t n) {
    f.clear();
    f.seekg(std::streamoff(offset));
    f.read(static_cast<char*>(dst), std::streamsize(n));
    return size_t(f.gcount()) == n;
}

// One directory table. It is a binary tree; walking every entry in table order visits each exactly once.
bool Walk(std::ifstream& f, Image& img, u32 sector, u32 size, const std::string& rel, int depth) {
    if (size == 0 || depth > 16 || size > (64u << 20))
        return true;
    std::vector<u8> table(size);
    if (!ReadAt(f, img.partition + u64(sector) * kSector, table.data(), size))
        return false;
    size_t pos = 0;
    while (pos + 14 <= table.size()) {
        if (Le16(&table[pos]) == 0xFFFF) { // padding up to the next sector
            pos = (pos / kSector + 1) * kSector;
            continue;
        }
        const u32 start = Le32(&table[pos + 4]), length = Le32(&table[pos + 8]);
        const u8 attr = table[pos + 12], nlen = table[pos + 13];
        if (nlen == 0 || pos + 14 + nlen > table.size())
            break;
        Entry e;
        e.path = rel + std::string(reinterpret_cast<const char*>(&table[pos + 14]), nlen);
        e.offset = img.partition + u64(start) * kSector;
        e.size = length;
        e.dir = (attr & 0x10) != 0;
        img.entries.push_back(e);
        if (e.dir) {
            if (!Walk(f, img, start, length, e.path + "\\", depth + 1))
                return false;
        } else {
            img.total += length;
        }
        if (img.entries.size() > 200000)
            return false;
        pos = (pos + 14 + nlen + 3) & ~size_t(3);
    }
    return true;
}

} // namespace

const Entry* Image::Find(const std::string& name) const {
    const std::string want = util::Lower(name);
    for (const Entry& e : entries)
        if (util::Lower(e.path) == want)
            return &e;
    return nullptr;
}

bool Open(const fs::path& iso, Image& image, std::string& error) {
    image = {};
    image.file = iso;
    std::ifstream f(iso, std::ios::binary);
    if (!f) {
        error = "The file could not be opened.";
        return false;
    }
    bool found = false;
    u8 vd[32];
    for (u64 off : kOffsets) {
        if (ReadAt(f, off + 32 * kSector, vd, sizeof(vd)) && std::memcmp(vd, kMagic, 20) == 0) {
            image.partition = off;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "Not an Xbox 360 disc image.";
        return false;
    }
    if (!Walk(f, image, Le32(vd + 20), Le32(vd + 24), "", 0) || image.entries.empty()) {
        error = "The disc image is damaged (its file table could not be read).";
        return false;
    }
    return true;
}

std::string Sha1(const Image& image, const Entry& e) {
    std::ifstream f(image.file, std::ios::binary);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (!f || BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        f.seekg(std::streamoff(e.offset));
        u64 left = e.size;
        bool ok = true;
        while (left && ok) {
            const size_t n = size_t(std::min<u64>(left, buf.size()));
            f.read(buf.data(), std::streamsize(n));
            ok = size_t(f.gcount()) == n;
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(n), 0);
            left -= n;
        }
        u8 digest[20];
        if (ok && BCryptFinishHash(hash, digest, 20, 0) == 0)
            for (u8 b : digest)
                hex += util::Format("%02x", b);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool Extract(const Image& image, const fs::path& out, const std::vector<std::string>& skip,
             const std::function<void(u64, u64)>& progress, std::atomic<bool>& cancel, std::string& error) {
    std::ifstream f(image.file, std::ios::binary);
    if (!f) {
        error = "The disc image could not be opened.";
        return false;
    }
    auto skipped = [&](const std::string& path) {
        const std::string top = util::Lower(path.substr(0, path.find('\\')));
        for (const std::string& s : skip)
            if (util::Lower(s) == top)
                return true;
        return false;
    };
    u64 total = 0, done = 0;
    for (const Entry& e : image.entries)
        if (!e.dir && !skipped(e.path))
            total += e.size;
    std::error_code ec;
    fs::create_directories(out, ec);
    std::vector<char> buf(8 << 20);
    for (const Entry& e : image.entries) {
        if (cancel) {
            error = "Cancelled.";
            return false;
        }
        if (skipped(e.path))
            continue;
        const fs::path dst = out / fs::path(util::Wide(e.path));
        if (e.dir) {
            fs::create_directories(dst, ec);
            continue;
        }
        fs::create_directories(dst.parent_path(), ec);
        if (fs::exists(dst, ec) && fs::file_size(dst, ec) == e.size) { // an earlier, interrupted run
            done += e.size;
            progress(done, total);
            continue;
        }
        fs::path part = dst;
        part += ".part";
        {
            std::ofstream o(part, std::ios::binary | std::ios::trunc);
            f.clear();
            f.seekg(std::streamoff(e.offset));
            u64 left = e.size;
            while (left) {
                if (cancel) {
                    o.close();
                    fs::remove(part, ec);
                    error = "Cancelled.";
                    return false;
                }
                const size_t n = size_t(std::min<u64>(left, buf.size()));
                f.read(buf.data(), std::streamsize(n));
                if (size_t(f.gcount()) != n) {
                    error = "The disc image is cut short: " + e.path + " could not be read.";
                    return false;
                }
                o.write(buf.data(), std::streamsize(n));
                if (!o) {
                    error = "Could not write " + util::Utf8(dst) + " (is the disk full?)";
                    return false;
                }
                left -= n;
                done += n;
                progress(done, total);
            }
        }
        fs::remove(dst, ec);
        fs::rename(part, dst, ec);
        if (ec) {
            error = "Could not write " + util::Utf8(dst);
            return false;
        }
    }
    return true;
}

} // namespace xiso
