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
    "tracy":         "https://github.com/wolfpld/tracy.git",
}
# El visor de Tracy del PC y el cliente del port deben ser exactamente de la misma versión.
TRACY_VERSION = "v0.15.0"

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
def tracy(): return FUENTES / "tracy"

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
    tiene = (tracy() / "public/TracyClient.cpp").is_file()
    print(f"  [{'x' if tiene else ' '}] fuentes/tracy {TRACY_VERSION} (fetch; opcional: build --tracy y perf)")
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
        branch = (["--branch", "rg35xxh-arm64"] if nombre == "cactus-galaxy" else
                  ["--branch", TRACY_VERSION] if nombre == "tracy" else [])
        if not dst.exists():
            run(["git", "clone", "--depth", "1", *branch, url, dst])
        if nombre == "DolRecomp":
            run(["git", "submodule", "update", "--init", "--recursive"], cwd=dst)
        elif nombre == "tracy":
            if run(["git", "describe", "--tags", "--exact-match"], cwd=dst, capture=True, check=False).stdout.strip() != TRACY_VERSION:
                print(f"Aviso: {dst} no está en {TRACY_VERSION}; el visor del PC debe ser de su misma versión.")
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
    # Descompresores de los REL (--dump-rels), compartidos con el host.
    src = '#include <cctype>\n#include "cactus_comprimidos.h"\n' + src
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
    # Módulos REL (sueltos o en Yaz0/LZ77), descomprimidos para DolRecomp. El host los vuelve a leer del
    # game.pak en la consola: el paquete no lleva datos del juego.
    rels = dj / "rels"
    shutil.rmtree(rels, ignore_errors=True); rels.mkdir()
    lista = run([exe, "--dump-rels", iso, rels], capture=True).stdout
    perfil["rels"] = dict(reversed(l.split("\t", 1)) for l in lista.splitlines() if "\t" in l)
    if perfil["rels"]:
        print(f"{len(perfil['rels'])} módulos REL: se recompilan junto al DOL (recomp).")
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
    base = list(cmd)
    if args.map:    cmd += ["--map", args.map]
    if args.config: cmd += ["--config", args.config]
    gid = perfil["id"]
    gen = dj / "recomp" / f"{gid}_generated"
    gen.mkdir(parents=True, exist_ok=True)
    run(cmd + [dj / "main.dol", gid, gen / f"{gid}.c"])
    recomp_rels(exe, base, perfil, dj, gen)
    print(f"C generado en {gen}")

# ---------------------------------------------------------------- REL
REL_BASE, REL_FIN = 0x81800000, 0x82000000   # hueco sobre MEM1 a menos de 32 MiB del DOL (wii_rel.h)

def recomp_rels(exe, cmd, perfil, dj, gen):
    """Recompila los REL del perfil (todos juntos, para resolver sus importaciones) y genera la tabla del host."""
    rels = perfil.get("rels") or {}
    for viejo in gen.glob("cactus_rel*"): viejo.unlink()
    if not rels:
        return
    out = dj / "recomp" / "rels"
    shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
    salida = run(cmd + ["--rel-base", hex(REL_BASE), dj / "rels", perfil["id"], out], capture=True).stdout
    modulos = leer_modulos(salida, dj / "rels", rels)
    if len(modulos) != len(rels):
        sys.exit(f"!! DolRecomp informó {len(modulos)} módulos REL de {len(rels)}")
    generar_tabla_rels(gen, out, modulos)

def leer_modulos(salida, dir_rels, rels):
    """Módulos que DolRecomp asignó en modo carpeta ('module N: ruta -> base 0x...'), con su ruta en el disco."""
    modulos = []
    for m in re.finditer(r"module (\d+): (.+?) -> base 0x([0-9A-Fa-f]{8})", salida):
        ruta = Path(m.group(2)).resolve().relative_to(Path(dir_rels).resolve()).as_posix()
        modulos.append({"id": int(m.group(1)), "base": int(m.group(3), 16), "rel": ruta,
                        "disco": rels.get(ruta, ruta)})
    return modulos

def generar_tabla_rels(gen, out, modulos):
    """cactus_rels.c (tabla id/base/ruta/búsqueda), un pegamento por módulo y cactus_rels.cmake con las fuentes."""
    fuentes, decl, filas = [], [], []
    for m in sorted(modulos, key=lambda m: m["base"]):
        tope = m["base"]
        nombre = f"{Path(m['rel']).stem}_{m['id']}"
        dirs = [d for d in out.rglob(nombre) if (d / "generated.c").is_file()]
        if len(dirs) != 1: sys.exit(f"!! no encuentro el C generado del módulo {m['id']} ({nombre})")
        d = dirs[0].resolve()
        fuentes.append(d / "generated.c")
        for linea in (d / "generated.c").read_text().splitlines():
            if re.match(r"^// chunks/[^ ]+\.c$", linea): fuentes.append(d / linea[3:])
        pegamento = gen / f"cactus_rel_{m['id']}.c"
        pegamento.write_text(f'#include "{d / "generated.h"}"\n'
                             f"DolRecompFunction cactus_rel_find_{m['id']}(u32 address) "
                             "{ return dolrecomp_find_original(address); }\n")
        fuentes.append(pegamento.resolve())
        decl.append(f"CactusRelFunction cactus_rel_find_{m['id']}(uint32_t);")
        disco = m["disco"].replace("\\", "\\\\").replace('"', '\\"')
        filas.append(f'    {{{m["id"]}u, 0x{m["base"]:08X}u, "{disco}", cactus_rel_find_{m["id"]}}},')
        if not (REL_BASE <= tope < REL_FIN):
            sys.exit(f"!! el módulo {m['id']} quedó en 0x{tope:08X}, fuera de 0x{REL_BASE:08X}-0x{REL_FIN:08X}")
    tabla = gen / "cactus_rels.c"
    tabla.write_text("// Generado por cactus-porter: módulos REL recompilados (wii_rel.h).\n"
                     "#include <stdint.h>\ntypedef struct CPUState CPUState;\n"
                     "typedef void (*CactusRelFunction)(CPUState*);\n"
                     "typedef struct { uint32_t id, base; const char* disc_path; "
                     "CactusRelFunction (*find)(uint32_t); } CactusRelModule;\n" +
                     "\n".join(decl) + "\nconst CactusRelModule cactus_rel_modules[] = {\n" + "\n".join(filas) +
                     f"\n}};\nconst unsigned cactus_rel_module_count = {len(filas)};\n")
    fuentes.append(tabla.resolve())
    (gen / "cactus_rels.cmake").write_text(
        "# Generado por cactus-porter: C de los REL recompilados.\nset(CACTUS_REL_SOURCES\n" +
        "".join(f'    "{f.as_posix()}"\n' for f in fuentes) + ")\n")
    print(f"{len(modulos)} módulos REL recompilados entre 0x{REL_BASE:08X} y 0x{REL_FIN:08X}.")

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
    shutil.copy2(PLANTILLA / "host/cactus_comprimidos.h", (root / pak).parent / "cactus_comprimidos.h")
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
    con_tracy = bool(getattr(args, "tracy", False))
    if con_tracy and not (tracy() / "public/TracyClient.cpp").is_file():
        sys.exit("!! falta fuentes/tracy: ejecuta ./cactus-porter.py fetch")
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
    # Con --tracy, en su propia carpeta: el host normal no cambia.
    bh = dj / ("build-host-tracy" if con_tracy else "build-host")
    run(["cmake", "-S", PLANTILLA/"host", "-B", bh, *gen,
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
         f"-DCACTUS_GALAXY_ROOT={fork()}", f"-DDOLRECOMP_DIR={dolr()}",
         f"-DGENERATED_DIR={dj/'recomp'/(perfil['id']+'_generated')}",
         f"-DCACTUS_SLUG={perfil['slug']}", f"-DCACTUS_SDL2_PREFIX={sdl}",
         f"-DCACTUS_TRACY={'ON' if con_tracy else 'OFF'}", f"-DTRACY_DIR={tracy()}"], env=env)
    run(["cmake", "--build", bh, "--target", f"{perfil['slug']}-host"], env=env)
    print("\nEjecutables aarch64 listos (nebula-pak del juego, wrapper y host" +
          (f", con Tracy {TRACY_VERSION}" if con_tracy else "") + ").")

# ---------------------------------------------------------------- package
def cmd_package(args):
    perfil = cargar_perfil(args.juego)
    dj = dir_juego(args.juego)
    slug, gid = perfil["slug"], perfil["id"]
    con_tracy = bool(getattr(args, "tracy", False))
    build_host = "build-host-tracy" if con_tracy else "build-host"
    puerto = dj / ("puerto-tracy" if con_tracy else "puerto") / slug
    gd = puerto / slug
    shutil.rmtree(puerto, ignore_errors=True)
    gd.mkdir(parents=True)
    (gd / "gamedata").mkdir(); (gd / "nand").mkdir()
    for binario, src in [("nebula-pak", dj/"build-pak-perfil/nebula-pak"),
                         ("cactus-galaxy-wrapper", dj/"build-pak-perfil/cactus-galaxy-wrapper"),
                         (f"{slug}-host", dj/build_host/f"{slug}-host")]:
        if not src.exists():
            sys.exit(f"!! falta {src}; ejecuta antes: build --juego {gid}" + (" --tracy" if con_tracy else ""))
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
    zip_path = SALIDA / (f"{slug}-tracy.zip" if con_tracy else f"{slug}.zip")
    with __import__("zipfile").ZipFile(zip_path, "w", __import__("zipfile").ZIP_DEFLATED) as z:
        for f in sorted(puerto.rglob("*")):
            if f.is_file(): z.write(f, f.relative_to(puerto))
    print(f"\nPort listo: {zip_path}\nInstálalo con PortMaster (KNULLI/muOS) y pon tu disco en ports/{slug}/gamedata/.")
    if con_tracy:
        print(f"Con Tracy {TRACY_VERSION}: abre el juego en la consola y, en el PC, "
              f"./cactus-porter.py perf --juego {gid} --ip <IP de la consola> (o el visor tracy-profiler).")

# ---------------------------------------------------------------- perf (Tracy)
def herramientas_tracy():
    """tracy-capture y tracy-csvexport para este PC, de la misma versión que el cliente del port."""
    if not (tracy() / "capture").is_dir():
        sys.exit("!! falta fuentes/tracy: ejecuta ./cactus-porter.py fetch")
    gen = ["-G", "Ninja"] if shutil.which("ninja") else []
    salida = {}
    for prog, carpeta in (("tracy-capture", "capture"), ("tracy-csvexport", "csvexport")):
        b = TRABAJO / "tracy-pc" / carpeta
        exe = b / prog
        if not exe.exists():
            run(["cmake", "-S", tracy() / carpeta, "-B", b, *gen, "-DCMAKE_BUILD_TYPE=Release"])
            run(["cmake", "--build", b, "--target", prog, "-j", str(os.cpu_count() or 4)])
        salida[prog] = exe
    return salida

def _csv(exe, args, traza):
    import csv, io
    out = run([exe, *args, traza], capture=True, check=False).stdout
    return list(csv.DictReader(io.StringIO(out)))

def resumen_traza(csvexport, traza):
    """Resumen legible de una captura: zonas, cuadros, gráficas y mensajes."""
    import statistics
    lineas = []
    zonas = _csv(csvexport, [], traza)
    marcos = _csv(csvexport, ["-F"], traza)
    eventos = _csv(csvexport, ["-u", "-p"], traza)
    mensajes = _csv(csvexport, ["-m"], traza)
    nombres_zona = {z["name"] for z in zonas}
    if marcos:
        f = marcos[0]
        # La mediana: el primer cuadro incluye la espera hasta que se conectó la captura.
        mediana = float(f["median_ns"]) / 1e6
        lineas.append(f"Cuadros: {f['frames']}, mediana {mediana:.2f} ms ({1000.0 / mediana if mediana else 0:.1f} fps), "
                      f"p90 {float(f['p90_ns'])/1e6:.2f} ms, p99 {float(f['p99_ns'])/1e6:.2f} ms")
    graficas = {}
    for e in eventos:
        if e.get("value") not in (None, "") and e["name"] not in nombres_zona and e["name"] != "Frame":
            graficas.setdefault(e["name"], []).append(float(e["value"]))
    if graficas:
        lineas.append("")
        lineas.append(f"{'Gráfica':42} {'media':>9} {'mín':>9} {'p10':>9} {'máx':>9}")
        for nombre, v in sorted(graficas.items()):
            o = sorted(v)
            lineas.append(f"{nombre[:42]:42} {statistics.fmean(v):9.2f} {o[0]:9.2f} {o[len(o)//10]:9.2f} {o[-1]:9.2f}")
    if zonas:
        lineas.append("")
        lineas.append(f"{'Zona':42} {'% del tiempo':>12} {'veces':>8} {'media ms':>9} {'peor ms':>9}")
        for z in sorted(zonas, key=lambda z: -float(z["total_perc"])):
            lineas.append(f"{z['name'][:42]:42} {float(z['total_perc']):12.1f} {int(z['counts']):8d} "
                          f"{float(z['mean_ns'])/1e6:9.3f} {float(z['max_ns'])/1e6:9.3f}")
    cpu = graficas.get("CPU: % del retrace emulado")
    audio = graficas.get("audio en cola (ms)")
    lineas.append("")
    if cpu:
        lineas.append(f"Velocidad de la CPU emulada: {statistics.fmean(cpu):.0f} % de una Wii de media "
                      f"(peor 10 %: {sorted(cpu)[len(cpu)//10]:.0f} %). Por debajo de 100 % el juego va lento.")
    if audio:
        cortes = sum(1 for x in audio if x <= 0.5)
        lineas.append(f"Audio: {cortes} de {len(audio)} cuadros con la cola de SDL vacía (cortes)." if cortes
                      else "Audio: la cola de SDL nunca se vació.")
    if mensajes:
        lineas.append("")
        lineas.append("Últimos mensajes:")
        for m in mensajes[-15:]:
            lineas.append(f"  {float(m['total_ns'])/1e9:8.2f} s  {m['MessageName']}")
    return "\n".join(lineas)

def cmd_perf(args):
    """Captura el rendimiento de un port con Tracy desde el PC (la consola en la misma red)."""
    import time
    if not args.traza and not args.ip:
        sys.exit("!! falta --ip (la IP de la consola, en KNULLI: Ajustes de red) o --traza <archivo.tracy>")
    h = herramientas_tracy()
    d = (dir_juego(args.juego) if args.juego else TRABAJO / "perfiles") / "perfiles"
    d.mkdir(parents=True, exist_ok=True)
    traza = Path(args.traza) if args.traza else d / time.strftime("%Y%m%d-%H%M%S.tracy")
    if not args.traza:
        print(f"Esperando al port en {args.ip}:{args.puerto} (ábrelo en la consola); se capturan {args.segundos} s.")
        run([h["tracy-capture"], "-a", args.ip, "-p", str(args.puerto), "-o", traza, "-s", str(args.segundos), "-f"])
    texto = resumen_traza(h["tracy-csvexport"], traza)
    traza.with_suffix(".txt").write_text(texto + "\n")
    print("\n" + texto)
    print(f"\nTraza: {traza} (ábrela con tracy-profiler {TRACY_VERSION}); resumen: {traza.with_suffix('.txt')}")

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
        else:
            p.add_argument("--tracy", action="store_true",
                           help=f"host con el cliente de Tracy {TRACY_VERSION} para medir el rendimiento")
    p = sub.add_parser("perf", help="captura el rendimiento del port con Tracy (PC y consola en la misma red)")
    p.add_argument("--juego", help="Title ID: la traza se guarda en trabajo/<ID>/perfiles")
    p.add_argument("--ip", help="IP de la consola")
    p.add_argument("--puerto", type=int, default=8086)
    p.add_argument("--segundos", type=int, default=60)
    p.add_argument("--traza", help="resumir una traza .tracy ya capturada en vez de capturar")
    p = sub.add_parser("all"); p.add_argument("--iso", required=True); p.add_argument("--region", default="USA")
    p.add_argument("--map"); p.add_argument("--config")
    args = ap.parse_args()
    {"doctor": cmd_doctor, "fetch": cmd_fetch, "probe": cmd_probe, "recomp": cmd_recomp,
     "build": cmd_build, "package": cmd_package, "restore": cmd_restore, "all": cmd_all,
     "perf": cmd_perf}[args.cmd](args)

if __name__ == "__main__":
    main()
