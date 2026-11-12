// wii_dvd.h
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
namespace wii {
struct PakFile { std::string path; uint64_t offset; uint32_t size; };
class PakDisc {
 public:
  ~PakDisc() { if (f_) fclose(f_); }
  bool open(const char* path);
  bool read(uint64_t offset, size_t size, uint8_t* out) const;   // DMA del DI
  bool read_file(const std::string& path, std::vector<uint8_t>& out) const;
  const std::vector<PakFile>& files() const { return files_; }
  uint64_t dol_offset() const { return dol_off_; }
  uint32_t dol_size() const { return dol_size_; }
 private:
  FILE* f_ = nullptr;
  uint64_t size_ = 0, dol_off_ = 0;
  uint32_t dol_size_ = 0;
  std::vector<PakFile> files_;
};
}
