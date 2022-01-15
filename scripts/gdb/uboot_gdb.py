import sys
import gdb

# b relocate_done
# add-symbol-file u-boot '((gd_t *)$x18)->relocaddr'
# mon system_reset

def uboot_gdp():
    return int(gdb.parse_and_eval('$x18'))

def uboot_relocaddr():
    assert uboot_gdp() != 0
    return int(gdb.parse_and_eval("((gd_t *)$x18)->relocaddr"))

def uboot_add_relocated_syms():
    symfile_cmd = f"add-symbol-file '{gdb.progspaces()[0].filename}' '((gd_t *)$x18)->relocaddr'"
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
