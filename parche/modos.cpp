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
