#!/usr/bin/env python3
"""cactus-porter — porta juegos de Wii a la Anbernic RG35XX H (y familia H700).

PC: recompila con DolRecomp (ExpansionPak) y arma los ejecutables aarch64 sobre
la infraestructura de cactus-galaxy (fork de Nebula). Consola: el Cactus Galaxy
Wrapper + nebula-pak parcheado hacen el game.pak desde TU propio disco.
Nada de código ni datos de Nintendo viaja en el paquete.
"""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path

RAIZ      = Path(__file__).resolve().parent
PLANTILLA = RAIZ / "plantilla"
FUENTES   = RAIZ / "fuentes"
TRABAJO   = RAIZ / "trabajo"
SALIDA    = RAIZ / "salida"

REPOS = {
    "DolRecomp":     "https://github.com/ExpansionPak/DolRecomp.git",
    "cactus-galaxy": "https://github.com/leonelavelarv-debug/cactus-galaxy.git",
}

def run(cmd, cwd=None, env=None, check=True, capture=False):
    cmd = [str(c) for c in cmd]
    print("+", " ".join(cmd), flush=True)
    p = subprocess.run(cmd, cwd=cwd, env=env,
                       capture_output=capture, text=True)
    if check and p.returncode != 0:
        if capture: print(p.stdout, p.stderr)
        sys.exit(f"!! falló ({p.returncode}): {' '.join(cmd)}")
    return p

DEPS = FUENTES / "deps"

def fork():  return FUENTES / "cactus-galaxy"
def dolr():  return FUENTES / "DolRecomp"

# ---------------------------------------------------------------- doctor
def cmd_doctor(_):
    ok = True
    for tool in ("git", "cmake", "ninja", "g++", "python3", "clang", "clang++", "ld.lld"):
        tiene = shutil.which(tool) is not None
        print(f"  [{'x' if tiene else ' '}] {tool}")
        ok &= tiene
    for nombre in ("cactus-galaxy", "DolRecomp"):
        tiene = (FUENTES / nombre).is_dir()
        print(f"  [{'x' if tiene else ' '}] fuentes/{nombre} (fetch)")
        ok &= tiene
    deps_ok = all((DEPS / d).is_dir() for d in ("glslang", "SPIRV-Cross"))
    print(f"  [{'x' if deps_ok else ' '}] fuentes/deps: glslang y SPIRV-Cross (instalar-entorno-nebula-v2.sh)")
    ok &= deps_ok
    xt = os.environ.get("AARCH64_XTOOLS", "")
    xt_ok = bool(xt and (Path(xt) / "aarch64-rpi3-linux-gnu/sysroot").is_dir())
    ok &= xt_ok
    print(f"  [{'x' if xt_ok else ' '}] AARCH64_XTOOLS (crosstool-NG aarch64-rpi3-linux-gnu, glibc<=2.27)")
    sdl = os.environ.get("CACTUS_SDL2_PREFIX", "")
    sdl_ok = bool(sdl and (Path(sdl) / "include/SDL2/SDL.h").is_file() and
                  (Path(sdl) / "lib/libSDL2-2.0.so").is_file())
    ok &= sdl_ok
    print(f"  [{'x' if sdl_ok else ' '}] CACTUS_SDL2_PREFIX (SDL2 compilado para aarch64)")
    print("\nGuía: el toolchain y el SDL2 de aarch64 se preparan como en docs/RG35XXH.md de tu fork.")
    print("El perfil del juego sale de tu propio disco (probe); el .pak final lo hace la consola.")

    if not ok:
        raise SystemExit(1)

# ---------------------------------------------------------------- fetch
def cmd_fetch(_):
    FUENTES.mkdir(exist_ok=True)
    for nombre, url in REPOS.items():
        dst = FUENTES / nombre
        branch = ["--branch", "rg35xxh-arm64"] if nombre == "cactus-galaxy" else []
        if not dst.exists():
            run(["git", "clone", "--depth", "1", *branch, url, dst])
        if nombre == "DolRecomp":
            run(["git", "submodule", "update", "--init", "--recursive"], cwd=dst)
        elif run(["git", "branch", "--show-current"], cwd=dst, capture=True).stdout.strip() != "rg35xxh-arm64":
            sys.exit(f"!! Cambia {dst} a la rama rg35xxh-arm64 antes de compilar.")
    if not DEPS.is_dir():
        print("Falta fuentes/deps (glslang, SPIRV-Cross): ejecuta ./instalar-entorno-nebula-v2.sh")

# ------------------------------------------------- parche de nebula-pak
TMD_NEW = '''void validate_tmd(const std::vector<u8>& tmd) {  // cactus-porter: cualquier juego Wii
  if (tmd.size() < 0x208 || be32(tmd.data()) != 0x00010001) fail("los metadatos de la particion no son los de %s", kGameId);
  const u16 count = be16(tmd.data() + 0x1DE);
  if (count == 0 || tmd.size() < size_t(0x1E4) + size_t(count) * 0x24) fail("los metadatos de la particion estan incompletos");
  for (u16 i = 0; i < count; ++i) {
    const u8* record = tmd.data() + 0x1E4 + size_t(i) * 0x24;
    if (be16(record + 4) != 0) continue;
    const u16 type = be16(record + 6);
    if ((type == 1 || type == 3) && be64(record + 8) == kPartitionDataBytes) return;
  }
  fail("el contenido de la particion no es el de %s", kGameId);
}'''

def parchear_nebula_pak(src: str, perfil: dict, modos_cpp: str) -> str:
    """Convierte nebula_pak.cpp (RMGE01 fijo) en el convertidor del juego del perfil.
    Con part_bytes=0 queda una 'sonda' válida solo para --probe/--dump-dol/--identify."""
    gid    = perfil["id"]
    nombre = perfil["nombre"]
    region = perfil.get("region", "USA")
    sha    = perfil.get("dol_sha1") or "0" * 40
    pb     = int(perfil.get("part_bytes") or 0)
    def ancla(patron, etiqueta, flags=0):
        m = re.search(patron, src, flags)
        if not m:
            sys.exit(f"!! parche '{etiqueta}': ancla no encontrada en nebula_pak.cpp (¿cambió el fork?)")
        return m.group(0)
    def uno(viejo, nuevo, etiqueta):
        nonlocal src
        if src.count(viejo) != 1:
            sys.exit(f"!! parche '{etiqueta}': ancla no encontrada (¿cambió el fork?)")
        src = src.replace(viejo, nuevo, 1)
    uno('constexpr const char* kGameId = "RMGE01";',
        f'constexpr const char* kGameId = "{gid}";', "kGameId")
    uno(ancla(r'constexpr const char\* kDolSha1 = "[0-9a-f]{40}";', "kDolSha1"),
        f'constexpr const char* kDolSha1 = "{sha}";', "kDolSha1")
    uno(ancla(r'constexpr u64 kPartitionDataBytes = 0x[0-9a-fA-F]+;', "kPartitionDataBytes"),
        f'constexpr u64 kPartitionDataBytes = 0x{pb:x};', "kPartitionDataBytes")
    uno(ancla(r'void validate_tmd\(const std::vector<u8>& tmd\) \{.*?\n\}', "validate_tmd", re.S),
        TMD_NEW, "validate_tmd")
    ancla = '  if (argc != 3) {\n    fprintf(stderr, "uso: nebula-pak'
    uno(ancla, modos_cpp + ancla, "modos")
    src = src.replace("Super Mario Galaxy", nombre).replace("RMGE01", gid).replace("norteamericano", region)
    return src

def dir_juego(gid): return TRABAJO / gid

def cargar_perfil(gid):
    p = dir_juego(gid) / "perfil.json"
    if not p.exists(): sys.exit(f"!! no existe {p}; ejecuta antes: probe --iso <tu disco>")
    return json.loads(p.read_text())

# ---------------------------------------------------------------- probe
def cmd_probe(args):
    iso = Path(args.iso).resolve()
    if not iso.exists(): sys.exit(f"!! no existe {iso}")
    t = TRABAJO / "sonda"; t.mkdir(parents=True, exist_ok=True)
    # Sonda nativa del PC con el CMake del fork y su zlib/xz/zstd/bzip2 vendorizados (sin paquetes -dev).
    pak_src = preparar_pak({"id": "WII000", "nombre": "juego Wii", "part_bytes": 0}, t)
    b = t / "build"
    gen = ["-G", "Ninja"] if shutil.which("ninja") else []
    run(["cmake", "-S", pak_src, "-B", b, *gen, "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", b, "--target", "nebula-pak"])
    exe = b / "nebula-pak"
    out = run([exe, "--probe", iso], capture=True).stdout.rstrip("\n").split("\t")
    if len(out) != 5: sys.exit("!! --probe no devolvió el perfil (¿disco de Wii válido?)")
    gid, formato, nombre, sha, pb = out
    perfil = {"id": gid, "nombre": nombre, "region": args.region, "formato": formato,
              "dol_sha1": sha, "part_bytes": int(pb),
              "slug": re.sub(r"[^a-z0-9]+", "-", nombre.lower()).strip("-") or gid.lower()}
    dj = dir_juego(gid); dj.mkdir(parents=True, exist_ok=True)
    run([exe, "--dump-dol", iso, dj / "main.dol"])
    (dj / "perfil.json").write_text(json.dumps(perfil, indent=2, ensure_ascii=False))
    print(f"\nPerfil de {nombre} ({gid}): SHA-1 {sha[:12]}…, partición {int(pb)>>20} MiB")
    print(f"Guardado en {dj/'perfil.json'}; main.dol listo para DolRecomp.")
    return gid

# ---------------------------------------------------------------- recomp
def cmd_recomp(args):
    perfil = cargar_perfil(args.juego)
    dj = dir_juego(args.juego)
    b = TRABAJO / "dolrecomp-build"
    run(["cmake", "-S", dolr(), "-B", b, "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", b, "--target", "dolrecomp", "--config", "Release", "-j", str(os.cpu_count() or 4)])
    exe = b / "dolrecomp"
    cmd = [exe, f"-j{os.cpu_count() or 4}", "--cpu", "broadway"]
    if args.map:    cmd += ["--map", args.map]
    if args.config: cmd += ["--config", args.config]
    gid = perfil["id"]
    gen = dj / "recomp" / f"{gid}_generated"
    gen.mkdir(parents=True, exist_ok=True)
    run(cmd + [dj / "main.dol", gid, gen / f"{gid}.c"])
    print(f"C generado en {gen}")

# ---------------------------------------------------------------- build
def fuente_original(path):
    backup = path.with_suffix(path.suffix + ".cactus-orig")
    return (backup if backup.exists() else path).read_text()

def preparar_pak(perfil, dj):
    # El CMake del fork resuelve third_party tres niveles por encima del proyecto.
    root = dj / "fuentes-pak"
    root.mkdir(parents=True, exist_ok=True)
    deps = root / "third_party"
    if not deps.is_symlink():
        deps.symlink_to(fork() / "third_party", target_is_directory=True)
    for subdir in ("pak", "cactus"):
        src = fork() / "platform/knulli" / subdir
        shutil.copytree(src, root / "platform/knulli" / subdir, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("*.cactus-orig"))
    pak = Path("platform/knulli/pak/nebula_pak.cpp")
    modos = (PLANTILLA / "parche/modos.cpp").read_text()
    (root / pak).write_text(parchear_nebula_pak(fuente_original(fork()/pak), perfil, modos))
    wrapper = Path("platform/knulli/cactus/cactus_wrapper.cpp")
    (root / wrapper).write_text(fuente_original(fork()/wrapper).replace("Super Mario Galaxy", perfil["nombre"]))
    return (root / pak).parent

def cmd_restore(_):
    for p in fork().rglob("*.cactus-orig"):
        shutil.copy2(p, p.with_name(p.name.replace(".cactus-orig", "")))
        p.unlink()
    print("Fork restaurado a su estado original.")

def entorno_cross():
    env = dict(os.environ)
    if not all((DEPS / d).is_dir() for d in ("glslang", "SPIRV-Cross")):
        sys.exit("!! falta fuentes/deps (glslang, SPIRV-Cross): ejecuta ./instalar-entorno-nebula-v2.sh")
    if not env.get("AARCH64_XTOOLS"):
        sys.exit("!! falta AARCH64_XTOOLS (raíz del crosstool-NG aarch64-rpi3-linux-gnu)")
    if not env.get("CACTUS_SDL2_PREFIX"):
        sys.exit("!! falta CACTUS_SDL2_PREFIX (SDL2 de aarch64: include/SDL2 y lib/libSDL2-2.0.so)")
    return env

def cmd_build(args):
    perfil = cargar_perfil(args.juego)
    dj = dir_juego(args.juego)
    env = entorno_cross()
    toolchain = fork()/"platform/knulli/aarch64-toolchain.cmake"
    sdl = env["CACTUS_SDL2_PREFIX"]
    gen = ["-G", "Ninja"] if shutil.which("ninja") else []
    pak_src = preparar_pak(perfil, dj)
    # 1) convertidor del juego + wrapper (CMake del propio fork, third_party vendored)
    bp = dj / "build-pak-perfil"
    run(["cmake", "-S", pak_src, "-B", bp, *gen,
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
         f"-DCACTUS_SDL2_PREFIX={sdl}"], env=env)
    run(["cmake", "--build", bp], env=env)
    # 2) host: C generado + dr_cpu + glue, con el mismo toolchain
    bh = dj / "build-host"
    run(["cmake", "-S", PLANTILLA/"host", "-B", bh, *gen,
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
         f"-DCACTUS_GALAXY_ROOT={fork()}", f"-DDOLRECOMP_DIR={dolr()}",
         f"-DGENERATED_DIR={dj/'recomp'/(perfil['id']+'_generated')}",
         f"-DCACTUS_SLUG={perfil['slug']}", f"-DCACTUS_SDL2_PREFIX={sdl}"], env=env)
    run(["cmake", "--build", bh, "--target", f"{perfil['slug']}-host"], env=env)
    print("\nEjecutables aarch64 listos (nebula-pak del juego, wrapper y host).")

# ---------------------------------------------------------------- package
def cmd_package(args):
    perfil = cargar_perfil(args.juego)
    dj = dir_juego(args.juego)
    slug, gid = perfil["slug"], perfil["id"]
    puerto = dj / "puerto" / slug
    gd = puerto / slug
    shutil.rmtree(puerto, ignore_errors=True)
    gd.mkdir(parents=True)
    (gd / "gamedata").mkdir(); (gd / "nand").mkdir()
    for binario, src in [("nebula-pak", dj/"build-pak-perfil/nebula-pak"),
                         ("cactus-galaxy-wrapper", dj/"build-pak-perfil/cactus-galaxy-wrapper"),
                         (f"{slug}-host", dj/f"build-host/{slug}-host")]:
        if not src.exists(): sys.exit(f"!! falta {src}; ejecuta antes: build --juego {gid}")
        shutil.copy2(src, gd / binario); (gd / binario).chmod(0o755)
    lanzador = (PLANTILLA/"port/lanzador.sh").read_text()
    for k, v in [("@TITULO@", perfil["nombre"]), ("@ID@", gid), ("@SLUG@", slug)]:
        lanzador = lanzador.replace(k, v)
    # Sin : / \\ * ? " < > | (exFAT/FAT32 de la SD y Windows no los admiten en nombres de fichero).
    titulo_fs = re.sub(r'[\\/:*?"<>|]+', " -", perfil["nombre"]).strip(" .") or slug
    sh = puerto / f"{titulo_fs}.sh"; sh.write_text(lanzador); sh.chmod(0o755)
    port_json = {
        "version": 4, "name": f"{slug}.zip",
        "items": [sh.name, f"{slug}/"], "items_opt": None,
        "attr": {"title": perfil["nombre"], "porter": ["cactus-porter"],
                 "desc": f"Port nativo de {perfil['nombre']} (Wii, {gid}) recompilado con DolRecomp sobre cactus-galaxy/Nebula (GLES 3.2).",
                 "inst": f"Necesita tu propio disco de {perfil['nombre']} ({gid}: ISO, RVZ, WIA, WBFS, GCZ o CISO). "
                         f"Ponlo en ports/{slug}/gamedata/ o en roms/wii/ y abre el juego: el wrapper hace el game.pak en la consola.",
                 "genres": ["other"], "image": {}, "rtr": False, "exp": True,
                 "runtime": [], "reqs": [], "arch": ["aarch64"], "min_glibc": "2.27"}}
    (gd / "port.json").write_text(json.dumps(port_json, indent=2, ensure_ascii=False))
    (gd / "gamedata" / "LEEME.txt").write_text(
        f"Pon aquí tu disco de {perfil['nombre']} ({gid}).\n"
        "El Cactus Galaxy Wrapper lo convierte a game.pak la primera vez, en esta consola.\n"
        "También vale tenerlo en roms/wii/. Nada de esto se distribuye con el port.\n")
    SALIDA.mkdir(exist_ok=True)
    zip_path = SALIDA / f"{slug}.zip"
    with __import__("zipfile").ZipFile(zip_path, "w", __import__("zipfile").ZIP_DEFLATED) as z:
        for f in sorted(puerto.rglob("*")):
            if f.is_file(): z.write(f, f.relative_to(puerto))
    print(f"\nPort listo: {zip_path}\nInstálalo con PortMaster (KNULLI/muOS) y pon tu disco en ports/{slug}/gamedata/.")

def cmd_all(args):
    a = argparse.Namespace(juego=cmd_probe(args), map=args.map, config=args.config)
    cmd_recomp(a); cmd_build(a); cmd_package(a)

def main():
    ap = argparse.ArgumentParser(description="Porta juegos de Wii a la RG35XX H: PC recompila y arma; la consola hace el .pak.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("doctor"); sub.add_parser("fetch"); sub.add_parser("restore")
    p = sub.add_parser("probe"); p.add_argument("--iso", required=True); p.add_argument("--region", default="USA")
    for nombre in ("recomp", "build", "package"):
        p = sub.add_parser(nombre); p.add_argument("--juego", required=True, help="Title ID, p. ej. RMGE01")
        if nombre == "recomp":
            p.add_argument("--map", help="MAP del linker para símbolos legibles")
            p.add_argument("--config", help="config.toml de parches de DolRecomp")
    p = sub.add_parser("all"); p.add_argument("--iso", required=True); p.add_argument("--region", default="USA")
    p.add_argument("--map"); p.add_argument("--config")
    args = ap.parse_args()
    {"doctor": cmd_doctor, "fetch": cmd_fetch, "probe": cmd_probe, "recomp": cmd_recomp,
     "build": cmd_build, "package": cmd_package, "restore": cmd_restore, "all": cmd_all}[args.cmd](args)

if __name__ == "__main__":
    main()
