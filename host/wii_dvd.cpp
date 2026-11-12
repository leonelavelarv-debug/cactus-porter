// wii_dvd.cpp
#include "wii_dvd.h"
#include <cstring>
#include <algorithm>
namespace wii {
static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; }
bool PakDisc::open(const char* path) {
  if (f_) fclose(f_);
  files_.clear();
  size_ = dol_off_ = dol_size_ = 0;
  f_ = fopen(path, "rb");
  if (!f_) return false;
  if (fseeko(f_, 0, SEEK_END) != 0) return false;
  const off_t length = ftello(f_);
  if (length < 0) return false;
  size_ = uint64_t(length);
  uint8_t boot[0x440];
  if (!read(0, sizeof boot, boot)) return false;
  dol_off_ = uint64_t(be32(boot + 0x420)) << 2;
  const uint64_t fst_off = uint64_t(be32(boot + 0x424)) << 2;
  const uint64_t fst_size = uint64_t(be32(boot + 0x428)) << 2;
  if (!dol_off_ || !fst_off || fst_size < 12 || fst_size > (64u<<20)) return false;
  uint8_t dh[0x100];
  if (!read(dol_off_, sizeof dh, dh)) return false;
  dol_size_ = 0x100;
  for (int i = 0; i < 18; ++i) {
    const uint32_t o = be32(dh + 4*i), n = be32(dh + 0x90 + 4*i);
    if (n && (o < 0x100 || uint64_t(o) + n > size_ - dol_off_ ||
              uint64_t(o) + n > (64u << 20))) return false;
    if (n) dol_size_ = std::max<uint32_t>(dol_size_, o + n);
  }
  std::vector<uint8_t> fst(fst_size);
  if (!read(fst_off, fst_size, fst.data()) || fst[0] != 1) return false;
  const uint32_t count = be32(fst.data() + 8);
  if (count == 0 || count > fst_size / 12) return false;
  const size_t strings = size_t(count) * 12;
  auto name_at = [&](uint32_t off) -> std::string {
    if (strings + off >= fst.size()) return {};
    const char* s = (const char*)fst.data() + strings + off;
    return std::string(s, strnlen(s, fst.size() - strings - off));
  };
  std::vector<std::pair<uint32_t, std::string>> dirs;
  for (uint32_t i = 1; i < count; ++i) {
    while (!dirs.empty() && i >= dirs.back().first) dirs.pop_back();
    const uint8_t* e = fst.data() + i * 12;
    const std::string name = name_at(be32(e) & 0xFFFFFF);
    const std::string full = dirs.empty() ? name : dirs.back().second + "/" + name;
    if (name.empty()) return false;
    if (e[0] == 1 && (be32(e+8) <= i || be32(e+8) > count)) return false;
    if (e[0] == 1) dirs.push_back({be32(e + 8), full});
    else {
      const uint64_t offset = uint64_t(be32(e + 4)) << 2;
      const uint32_t length = be32(e + 8);
      if (offset > size_ || length > size_ - offset) return false;
      files_.push_back({full, offset, length});
    }
  }
  return true;
}
bool PakDisc::read(uint64_t offset, size_t size, uint8_t* out) const {
  if (!f_ || offset > size_ || size > size_ - offset) return false;
  if (fseeko(f_, (off_t)offset, SEEK_SET) != 0) return false;
  return fread(out, 1, size, f_) == size;
}
bool PakDisc::read_file(const std::string& path, std::vector<uint8_t>& out) const {
  for (const PakFile& f : files_)
    if (f.path == path) { out.resize(f.size); return read(f.offset, f.size, out.data()); }
  return false;
}
}
