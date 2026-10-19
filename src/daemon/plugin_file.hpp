// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../../sdk/plugin/kabuki_plugin.h"
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <vector>
namespace kabuki {
// Validate every range before the existing mapper sees it. Plugins are
// freestanding ET_DYN files: no imports, TLS, constructors or executable entry
// calls.
inline bool plugin_file(const std::string &path,
                        std::vector<std::uint8_t> &bytes,
                        std::uint64_t &symbol) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    return false;
  const auto n = f.tellg();
  if (n < 64 || n > 2 * 1024 * 1024)
    return false;
  bytes.resize(std::size_t(n));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(bytes.data()), n))
    return false;
  const auto range = [&](std::uint64_t at, std::uint64_t size) {
    return at <= bytes.size() && size <= bytes.size() - at;
  };
  const auto &e = *reinterpret_cast<const Elf64_Ehdr *>(bytes.data());
  if (std::memcmp(e.e_ident, ELFMAG, SELFMAG) ||
      e.e_ident[EI_CLASS] != ELFCLASS64 || e.e_ident[EI_DATA] != ELFDATA2LSB ||
      e.e_type != ET_DYN || e.e_machine != EM_X86_64 ||
      e.e_phentsize != sizeof(Elf64_Phdr) ||
      e.e_shentsize != sizeof(Elf64_Shdr) || !e.e_phnum || !e.e_shnum ||
      !range(e.e_phoff, std::uint64_t(e.e_phnum) * sizeof(Elf64_Phdr)) ||
      !range(e.e_shoff, std::uint64_t(e.e_shnum) * sizeof(Elf64_Shdr)))
    return false;
  const auto *p =
      reinterpret_cast<const Elf64_Phdr *>(bytes.data() + e.e_phoff);
  bool entry = false;
  for (unsigned j = 0; j < e.e_phnum; ++j) {
    if (p[j].p_type == PT_INTERP || p[j].p_type == PT_TLS)
      return false;
    if (p[j].p_type != PT_LOAD)
      continue;
    if (!range(p[j].p_offset, p[j].p_filesz) || p[j].p_filesz > p[j].p_memsz ||
        p[j].p_vaddr > 4 * 1024 * 1024 ||
        p[j].p_memsz > 4 * 1024 * 1024 - p[j].p_vaddr)
      return false;
    if ((p[j].p_flags & PF_X) && e.e_entry >= p[j].p_vaddr &&
        e.e_entry - p[j].p_vaddr < p[j].p_memsz)
      entry = true;
  }
  if (!entry)
    return false;
  const auto *s =
      reinterpret_cast<const Elf64_Shdr *>(bytes.data() + e.e_shoff);
  bool found = false;
  for (unsigned j = 0; j < e.e_shnum; ++j) {
    if (s[j].sh_type != SHT_NOBITS && !range(s[j].sh_offset, s[j].sh_size))
      return false;
    if (s[j].sh_type == SHT_INIT_ARRAY || s[j].sh_type == SHT_FINI_ARRAY)
      return false;
    if (s[j].sh_type == SHT_RELA) {
      if (s[j].sh_entsize != sizeof(Elf64_Rela) ||
          s[j].sh_size % sizeof(Elf64_Rela))
        return false;
      const auto *r =
          reinterpret_cast<const Elf64_Rela *>(bytes.data() + s[j].sh_offset);
      for (std::size_t k = 0; k < s[j].sh_size / sizeof(*r); ++k) {
        if (ELF64_R_TYPE(r[k].r_info) != R_X86_64_RELATIVE ||
            r[k].r_offset > 4 * 1024 * 1024 - 8)
          return false;
        bool target = false;
        for (unsigned q = 0; q < e.e_phnum; ++q)
          if (p[q].p_type == PT_LOAD && r[k].r_offset >= p[q].p_vaddr &&
              r[k].r_offset - p[q].p_vaddr + 8 <= p[q].p_memsz)
            target = true;
        if (!target)
          return false;
      }
    }
    if (s[j].sh_type != SHT_DYNSYM)
      continue;
    if (s[j].sh_entsize != sizeof(Elf64_Sym) ||
        s[j].sh_size % sizeof(Elf64_Sym) || s[j].sh_link >= e.e_shnum)
      return false;
    const auto &names = s[s[j].sh_link];
    if (!range(names.sh_offset, names.sh_size))
      return false;
    const auto *syms =
        reinterpret_cast<const Elf64_Sym *>(bytes.data() + s[j].sh_offset);
    for (std::size_t k = 1; k < s[j].sh_size / sizeof(Elf64_Sym); ++k) {
      const auto &sym = syms[k];
      if (sym.st_shndx == SHN_UNDEF && sym.st_name)
        return false;
      if (sym.st_name >= names.sh_size)
        return false;
      const auto *name = reinterpret_cast<const char *>(
          bytes.data() + names.sh_offset + sym.st_name);
      if (!std::memchr(name, 0, std::size_t(names.sh_size - sym.st_name)))
        return false;
      if (std::strcmp(name, "kabuki_plugin_v1"))
        continue;
      if (sym.st_size < sizeof(kabuki_plugin))
        return false;
      for (unsigned q = 0; q < e.e_phnum; ++q) {
        if (p[q].p_type != PT_LOAD || sym.st_value < p[q].p_vaddr)
          continue;
        const auto delta = sym.st_value - p[q].p_vaddr;
        if (delta + sizeof(kabuki_plugin) > p[q].p_filesz)
          continue;
        const auto *api = reinterpret_cast<const kabuki_plugin *>(
            bytes.data() + p[q].p_offset + delta);
        if (api->abi != KABUKI_PLUGIN_ABI ||
            api->size < sizeof(kabuki_plugin) || api->size > sym.st_size)
          return false;
        symbol = sym.st_value;
        found = true;
      }
    }
  }
  return found;
}
} // namespace kabuki
