if (argc == 3 && strcmp(argv[1], "--probe") == 0) {  // cactus-porter: perfil del disco (PC)
    try {
      aes_init();
      std::unique_ptr<Disc> disc = open_disc(argv[2]);
      u8 header[0x60];
      disc->read(0, sizeof(header), header);
      const std::string id(reinterpret_cast<const char*>(header), 6);
      std::string name(reinterpret_cast<const char*>(header + 0x20),
                       strnlen(reinterpret_cast<const char*>(header + 0x20), 0x40));
      for (char& c : name) if (c == '\t' || c == '\n') c = ' ';
      GamePartition gp = locate_partition(*disc);
      std::unique_ptr<PartitionData> data = open_partition_data(*disc, gp);
      const std::vector<u8> dol = read_dol(*data);
      u64 part_bytes = 0;
      if (gp.tmd.size() >= 0x208) {
        const u16 n = be16(gp.tmd.data() + 0x1DE);
        for (u16 i = 0; i < n && size_t(0x1E4) + size_t(i + 1) * 0x24 <= gp.tmd.size(); ++i) {
          const u8* r = gp.tmd.data() + 0x1E4 + size_t(i) * 0x24;
          if (be16(r + 4) == 0) { part_bytes = be64(r + 8); break; }
        }
      }
      printf("%s\t%s\t%s\t%s\t%llu\n", id.c_str(), disc->format(), name.c_str(),
             Sha1::hex(dol.data(), dol.size()).c_str(), (unsigned long long)part_bytes);
      return 0;
    } catch (const std::exception& error) { fprintf(stderr, "Error: %s\n", error.what()); return 1; }
  }
  if (argc == 4 && strcmp(argv[1], "--dump-dol") == 0) {  // cactus-porter: main.dol para DolRecomp
    try {
      aes_init();
      std::unique_ptr<Disc> disc = open_disc(argv[2]);
      GamePartition gp = locate_partition(*disc);
      std::unique_ptr<PartitionData> data = open_partition_data(*disc, gp);
      const std::vector<u8> dol = read_dol(*data);
      FILE* f = fopen(argv[3], "wb");
      if (!f || fwrite(dol.data(), 1, dol.size(), f) != dol.size()) { if (f) fclose(f); return 1; }
      fclose(f);
      return 0;
    } catch (const std::exception& error) { fprintf(stderr, "Error: %s\n", error.what()); return 1; }
  }
  if (argc == 4 && strcmp(argv[1], "--dump-rels") == 0) {  // cactus-porter: módulos REL para DolRecomp
    try {
      aes_init();
      std::unique_ptr<Disc> disc = open_disc(argv[2]);
      GamePartition gp = locate_partition(*disc);
      std::unique_ptr<PartitionData> data = open_partition_data(*disc, gp);
      u8 boot[0x440];
      data->read(0, sizeof(boot), boot);
      const u64 fst_offset = u64(be32(boot + 0x424)) << 2, fst_size = u64(be32(boot + 0x428)) << 2;
      if (fst_size < 12 || fst_size > (64u << 20)) fail("la tabla de ficheros del disco no es valida");
      std::vector<u8> fst(static_cast<size_t>(fst_size));
      data->read(fst_offset, fst.size(), fst.data());
      const std::string out_dir = argv[3];
      for (const FileEntry& f : parse_fst(fst)) {
        std::string lower = f.name;
        for (char& c : lower) c = char(tolower((unsigned char)c));
        const bool compressed_name = lower.size() > 7 &&
            (lower.rfind(".rel.lz") == lower.size() - 7 || lower.rfind(".rel.szs") == lower.size() - 8 ||
             lower.rfind(".rel.yaz0") == lower.size() - 9);
        const bool plain = lower.size() > 4 && lower.rfind(".rel") == lower.size() - 4;
        if ((!plain && !compressed_name) || f.length == 0 || f.length > (64u << 20)) continue;
        std::vector<u8> raw(static_cast<size_t>(f.length)), rel;
        data->read(f.offset, raw.size(), raw.data());
        if (!cactus_lz::rel_bytes(raw, rel)) { fprintf(stderr, "Aviso: %s no es un REL legible\n", f.name.c_str()); continue; }
        std::string out_rel = f.name;
        if (!plain) out_rel = out_rel.substr(0, out_rel.size() - (lower.size() - lower.rfind(".rel") - 4));
        std::string path = out_dir + "/" + out_rel;
        for (size_t i = out_dir.size() + 1; i < path.size(); ++i) {
          if (path[i] != '/') continue;
          path[i] = 0; mkdir(path.c_str(), 0755); path[i] = '/';
        }
        FILE* o = fopen(path.c_str(), "wb");
        if (!o || fwrite(rel.data(), 1, rel.size(), o) != rel.size()) { if (o) fclose(o); fail("no se pudo escribir %s", path.c_str()); }
        fclose(o);
        printf("%s\t%s\n", f.name.c_str(), out_rel.c_str());
      }
      return 0;
    } catch (const std::exception& error) { fprintf(stderr, "Error: %s\n", error.what()); return 1; }
  }
