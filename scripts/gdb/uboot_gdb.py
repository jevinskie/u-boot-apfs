import sys
import gdb

from elftools.elf.elffile import ELFFile
from elftools.elf.constants import SH_FLAGS

# b relocate_done
# add-symbol-file u-boot '((gd_t *)$x18)->relocaddr'
# mon system_reset

def uboot_gdp():
    return int(gdb.parse_and_eval('$x18'))

def uboot_relocaddr():
    assert uboot_gdp() != 0
    return int(gdb.parse_and_eval("((gd_t *)$x18)->relocaddr"))

def uboot_add_relocated_syms():
    elf_file = gdb.progspaces()[0].filename
    relocaddr = uboot_relocaddr()

    elf = ELFFile(open(elf_file, 'rb'))
    secs_offs = {}
    text_off = None
    for sec_idx in range(elf.header.e_shnum):
        sec = elf.get_section(sec_idx)
        if sec.header.sh_flags & SH_FLAGS.SHF_ALLOC:
            if sec.name == ".text":
                text_off = sec.header.sh_addr
                assert not len(secs_offs)
            secs_offs[sec.header.sh_addr - text_off] = sec.name

    symfile_cmd = f"add-symbol-file '{elf_file}' -readnow {hex(relocaddr)} "
    sub_cmds = []
    for sec_off in sorted(secs_offs.keys())[1:]:
        sub_cmds.append(f"-s {secs_offs[sec_off]} {hex(sec_off + relocaddr)}")
    symfile_cmd += " ".join(sub_cmds)

    # print(secs_offs)
    # print(symfile_cmd)

    # symfile_cmd = f"add-symbol-file '{elf_file}' -readnow -o {hex(relocaddr)} {hex(relocaddr)}"
    # symfile_cmd = f"add-symbol-file '{elf_file}' -readnow {hex(relocaddr)}"
    gdb.execute(symfile_cmd, False, True)

class UBootRelocateBP(gdb.Breakpoint):
    def stop(self):
        uboot_add_relocated_syms()
        return False

class UBootRelocate(gdb.Command):
    """Set up GDB to handle U-Boot relocating itself"""

    def __init__(self):
        super().__init__("uboot-relocate", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        x18 = uboot_gdp()
        if x18:
            uboot_add_relocated_syms()
        else:
            UBootRelocateBP('relocate_done', gdb.BP_BREAKPOINT, gdb.WP_WRITE, True, True)

UBootRelocate()

class UBootUnslide(gdb.Command):
    """Set up GDB to handle U-Boot relocating itself"""

    def __init__(self):
        super().__init__("uboot-unslide", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        x18 = uboot_gdp()
        reloc_addr = gdb.parse_and_eval(f"{arg}")
        orig_addr = reloc_addr
        if x18:
            orig_addr -= uboot_relocaddr()
        print(f"{hex(reloc_addr)} -> {hex(orig_addr)}")
        gdb.execute(f"info symbol {hex(orig_addr)}")

UBootUnslide()

class AddSymFileSmartELF(gdb.Command):
    """Set up GDB to handle U-Boot relocating itself"""

    def __init__(self):
        super().__init__("add-symbol-file-smart-elf", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        x18 = uboot_gdp()
        if x18:
            uboot_add_relocated_syms()
        else:
            UBootRelocateBP('relocate_done', gdb.BP_BREAKPOINT, gdb.WP_WRITE, True, True)

UBootRelocate()

