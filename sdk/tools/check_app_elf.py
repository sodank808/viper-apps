#!/usr/bin/env python3
"""Check that a built .app.elf can be loaded by Viper's ELF loader, without running it.

The loader (src/esp_elf.c, src/arch/esp_elf_xtensa.c) only loads a few sections
by name, handles a few relocation types, and resolves undefined symbols from
fixed export tables. An app that breaks one of those rules fails to load on the
device, or before the loader reported relocation errors, loaded with null
pointers. This script applies the same rules on the host:

  - the file is a 32-bit little-endian Xtensa shared object
  - the entry point is in .text
  - every relocation type is one esp_elf_arch_relocate() handles
  - every relocation's target address is inside a section the loader loads
  - every R_XTENSA_RELATIVE pointer points into a loaded section, or one past
    the end of one (an array's end pointer), as esp_elf_map_data_ptr() allows
  - every undefined symbol a relocation needs is in an export table

Export tables are read from the loader's sources: the libc and ESP-IDF tables in
src/esp_elf_symbol.c, and Viper's exports from exports/*.txt, the lists the
firmware build generates its table from (gen_exports.py). In the app SDK
(scripts/export_sdk.py), which has no loader sources, the first two come from
exports/builtin.list beside exports/*.txt instead. This assumes
CONFIG_ELF_LOADER_LIBC_SYMBOLS,
CONFIG_ELF_LOADER_ESPIDF_SYMBOLS and CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS are on,
as they are in sdkconfig.base. Keep the rules in sync with the loader.

It also reports the app's API level: the highest level, in exports.lock
(beside exports/), of the firmware functions it calls; the loader's own libc
and ESP-IDF functions are level 1. That's the oldest firmware API it runs on;
install_app.sh writes it into the installed info.json as "min_api".

Usage: check_app_elf.py [--src <elf_loader/src>] <app.elf>...
       check_app_elf.py --api-level <app.elf>   (checks it, prints only its level)
       check_app_elf.py --builtin-list          (prints builtin.list for the SDK)
Exits 1 if any file has problems.
"""

import argparse
import pathlib
import re
import struct
import sys

import gen_exports  # same folder

EM_XTENSA = 94
ET_DYN = 3

SHT_PROGBITS = 1
SHT_RELA = 4
SHT_NOBITS = 8

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

R_XTENSA_NONE = 0
R_XTENSA_RTLD = 2
R_XTENSA_GLOB_DAT = 3
R_XTENSA_JMP_SLOT = 4
R_XTENSA_RELATIVE = 5
SUPPORTED_RELOCATIONS = {
    R_XTENSA_RTLD: "R_XTENSA_RTLD",
    R_XTENSA_GLOB_DAT: "R_XTENSA_GLOB_DAT",
    R_XTENSA_JMP_SLOT: "R_XTENSA_JMP_SLOT",
    R_XTENSA_RELATIVE: "R_XTENSA_RELATIVE",
}

DEFAULT_SRC = pathlib.Path(__file__).resolve().parent.parent / "src"

# The SDK's copy of the loader's own tables (the in-tree tool/ and the SDK's
# tools/ both sit beside exports/)
BUILTIN_LIST = "builtin.list"


class Section:
    def __init__(self, name, sh_type, flags, addr, offset, size, link):
        self.name = name
        self.type = sh_type
        self.flags = flags
        self.addr = addr
        self.offset = offset
        self.size = size
        self.link = link

    def contains(self, addr):
        return self.addr <= addr < self.addr + self.size


def is_loaded(sec):
    """Mirror the section matching in esp_elf_load_section()."""
    # C++ constructors and destructors, whatever their section type
    if sec.flags & SHF_ALLOC and sec.name in (".ctors", ".init_array", ".dtors", ".fini_array"):
        return True
    if sec.type == SHT_PROGBITS and sec.flags & SHF_ALLOC:
        executable = bool(sec.flags & SHF_EXECINSTR)
        writable = bool(sec.flags & SHF_WRITE)
        if executable and sec.name == ".text":
            return True
        if writable and sec.name == ".data":
            return True
        if sec.name in (".rodata", ".data.rel.ro"):
            return True
        if executable and sec.name.startswith(".shared_iram"):
            return True
        if writable and sec.name == ".shared_dram":
            return True
    if (sec.type == SHT_NOBITS and sec.flags & SHF_ALLOC and sec.flags & SHF_WRITE
            and sec.name == ".bss"):
        return True
    return False


# Table entries: ESP_ELFSYM_EXPORT(name), or { "name", <address> } for exports
# under another function (malloc -> esp_elf_app_malloc)
EXPORT_PATTERNS = (r"ESP_ELFSYM_EXPORT\((\w+)\)", r'\{\s*"(\w+)"\s*,')


def read_builtin_exports(src_dir):
    """The libc and ESP-IDF names the loader exports itself (esp_elf_symbol.c)."""
    text = (src_dir / "esp_elf_symbol.c").read_text()
    exports = set()
    for pattern in EXPORT_PATTERNS:
        exports.update(re.findall(pattern, text))
    return exports


def read_exports(src_dir):
    exports_dir = src_dir.parent / "exports"
    if (src_dir / "esp_elf_symbol.c").exists():
        exports = read_builtin_exports(src_dir)
    else:
        builtin = exports_dir / BUILTIN_LIST
        if not builtin.exists():
            sys.exit(f"neither {src_dir / 'esp_elf_symbol.c'} nor {builtin} found")
        exports = {line.strip() for line in builtin.read_text().splitlines()
                   if line.strip() and not line.startswith("#")}
    exports.update(gen_exports.read_exports(sorted(exports_dir.glob("*.txt"))))
    return exports


def read_levels(src_dir):
    """{name: API level} from exports.lock beside exports/ (empty if missing)."""
    lock = src_dir.parent / "exports.lock"
    levels = {}
    if lock.exists():
        for line in lock.read_text().splitlines():
            fields = line.split()
            if fields and not line.startswith("#"):
                levels[fields[0]] = int(fields[1]) if len(fields) > 1 else 1
    return levels


def api_level(imports, levels):
    """The API level an app calling `imports` needs: the highest of theirs,
    1 for names with none (the loader's own, or none at all)."""
    return max((levels.get(name, 1) for name in imports), default=1)


def builtin_list(src_dir):
    """builtin.list's text: a header, then the names, sorted."""
    lines = ["# The libc and ESP-IDF names Viper's ELF loader exports itself, from its",
             "# src/esp_elf_symbol.c. GENERATED by scripts/export_sdk.py; check_app_elf.py",
             "# reads it. Viper's own exports are in the *.txt files beside it."]
    return "\n".join(lines + sorted(read_builtin_exports(src_dir))) + "\n"


def c_string(data, offset):
    end = data.index(b"\0", offset)
    return data[offset:end].decode("utf-8", "replace")


def check(path, exports, imports=None):
    """Problems with the file (empty if none). Adds the names it imports from
    the firmware to `imports` when given."""
    data = path.read_bytes()
    problems = []

    if data[:4] != b"\x7fELF":
        return ["not an ELF file"]
    if data[4] != 1 or data[5] != 1:
        return ["not a 32-bit little-endian ELF"]

    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_machine != EM_XTENSA:
        problems.append(f"machine is {e_machine}, not Xtensa ({EM_XTENSA})")
    if e_type != ET_DYN:
        problems.append(f"ELF type is {e_type}, not a shared object ({ET_DYN})")
    if problems:
        return problems

    raw = []
    for i in range(e_shnum):
        raw.append(struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize))
    shstr_offset = raw[e_shstrndx][4]
    sections = [
        Section(c_string(data, shstr_offset + s[0]), s[1], s[2], s[3], s[4], s[5], s[6])
        for s in raw
    ]
    loaded = [s for s in sections if is_loaded(s) and s.size]

    def loaded_section(addr):
        for sec in loaded:
            if sec.contains(addr):
                return sec
        return None

    def pointer_section(addr):
        """Mirror esp_elf_map_data_ptr(): an address inside a section is that
        section's; otherwise one past a section's end is that section's end
        pointer. Returns (section, None), or (None, problem)."""
        sec = loaded_section(addr)
        if sec is not None:
            return sec, None
        ending = [s for s in loaded if s.addr + s.size == addr]
        if len(ending) > 1:
            names = " and ".join(s.name for s in ending)
            return None, f"0x{addr:x}, the end of both {names}; can't tell which it means"
        if ending:
            return ending[0], None
        return None, describe(addr)

    def describe(addr):
        for sec in sections:
            if sec.flags & SHF_ALLOC and sec.contains(addr):
                return f"0x{addr:x} (in {sec.name}, which the loader doesn't load)"
        return f"0x{addr:x} (not in any section)"

    text = next((s for s in loaded if s.name == ".text"), None)
    if text is None:
        problems.append("no .text section")
    elif not text.contains(e_entry):
        problems.append(f"entry point {describe(e_entry)} is not in .text")

    for rela_sec in (s for s in sections if s.type == SHT_RELA):
        symtab = sections[rela_sec.link]
        strtab = sections[symtab.link]
        for i in range(rela_sec.size // 12):
            r_offset, r_info, _ = struct.unpack_from("<IIi", data, rela_sec.offset + i * 12)
            r_type = r_info & 0xFF
            r_sym = r_info >> 8
            st_name, st_value = struct.unpack_from("<II", data, symtab.offset + r_sym * 16)
            sym_name = c_string(data, strtab.offset + st_name) if st_name else ""
            where = f"{rela_sec.name}[{i}] at 0x{r_offset:x}"

            if r_type == R_XTENSA_NONE:
                # Does nothing; the linker leaves these for dynamic relocations
                # it reserved and didn't need (libgcc's 64-bit division makes
                # some). The loader skips them.
                continue
            if r_type not in SUPPORTED_RELOCATIONS:
                problems.append(f"{where}: relocation type {r_type} isn't supported by the loader"
                                + (f" (symbol {sym_name})" if sym_name else ""))
                continue
            type_name = SUPPORTED_RELOCATIONS[r_type]

            target = loaded_section(r_offset)
            if target is None:
                problems.append(f"{where}: {type_name} target {describe(r_offset)}")
                continue

            if r_type == R_XTENSA_RELATIVE:
                if target.type == SHT_NOBITS:
                    problems.append(f"{where}: R_XTENSA_RELATIVE target is in {target.name}")
                    continue
                (pointer,) = struct.unpack_from(
                    "<I", data, target.offset + (r_offset - target.addr))
                _, problem = pointer_section(pointer)
                if problem:
                    problems.append(f"{where}: R_XTENSA_RELATIVE points to {problem}")
            elif r_type == R_XTENSA_GLOB_DAT and sym_name:
                if imports is not None:
                    imports.add(sym_name)
                if sym_name not in exports:
                    problems.append(f"{where}: {sym_name} isn't exported by the firmware")
            elif r_type == R_XTENSA_JMP_SLOT:
                if st_value:
                    if loaded_section(st_value) is None:
                        problems.append(f"{where}: {sym_name} is at {describe(st_value)}")
                else:
                    if imports is not None:
                        imports.add(sym_name)
                    if sym_name not in exports:
                        problems.append(f"{where}: {sym_name} isn't exported by the firmware")

    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf", nargs="*", type=pathlib.Path, help=".app.elf files to check")
    parser.add_argument("--src", type=pathlib.Path, default=DEFAULT_SRC,
                        help="elf_loader/src directory to read the export tables from")
    parser.add_argument("--builtin-list", action="store_true",
                        help="print the loader's own exports in builtin.list's format and exit")
    parser.add_argument("--api-level", action="store_true",
                        help="print only the highest API level the files need")
    args = parser.parse_args()
    if args.builtin_list:
        sys.stdout.write(builtin_list(args.src))
        return 0
    if not args.elf:
        parser.error("no .app.elf files given")

    exports = read_exports(args.src)
    levels = read_levels(args.src)
    failed = False
    highest = 1
    for path in args.elf:
        imports = set()
        problems = check(path, exports, imports)
        level = api_level(imports, levels)
        highest = max(highest, level)
        if problems:
            failed = True
            print(f"{path}: can't be loaded by Viper's ELF loader:", file=sys.stderr)
            for problem in problems:
                print(f"  {problem}", file=sys.stderr)
        elif not args.api_level:
            print(f"{path}: OK (API level {level})")
    if args.api_level and not failed:
        print(highest)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
