#!/usr/bin/env python3
"""Reject versioned ALSA compatibility aliases for stock-linked R1 clients.

Stock R1 libasound exposes the current PCM ABI without symbol versions.
An unversioned client can bind to ALSA_0.9 legacy getters in a versioned
replacement, even when ELF soname and all imported symbol names match.
"""
import sys
from elftools.elf.elffile import ELFFile

with open(sys.argv[1], "rb") as stream:
    elf = ELFFile(stream)
    definitions = elf.get_section_by_name(".gnu.version_d")
    versions = set()
    if definitions is not None:
        for _, auxiliaries in definitions.iter_versions():
            versions.update(item.name for item in auxiliaries)
    if any(name.startswith("ALSA_") for name in versions):
        raise SystemExit("ALSA ABI mismatch: rebuild with --without-versioned for stock R1 clients")
    symbols = elf.get_section_by_name(".dynsym")
    available = {symbol.name for symbol in symbols.iter_symbols()
                 if symbol["st_shndx"] != "SHN_UNDEF"}
    required = {"snd_asoundlib_version", "snd_pcm_open", "snd_pcm_close",
                "snd_pcm_hw_params_get_period_size", "snd_pcm_hw_params_get_buffer_size",
                "snd_pcm_ioplug_create", "snd_ctl_ext_create", "snd_use_case_mgr_open"}
    if not required <= available:
        raise SystemExit("ALSA candidate lacks required PCM/plugin/UCM APIs")
print("ALSA stock R1 unversioned ABI: PASS")
