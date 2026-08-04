/* Linker script to configure memory regions. */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)     : ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000

  /* SRAM required by Softdevice depend on
   * - Attribute Table Size (Number of Services and Characteristics)
   * - Vendor UUID count
   * - Max ATT MTU
   * - Concurrent connection peripheral + central + secure links
   * - Event Len, HVN queue, Write CMD queue
   */ 
  RAM (rwx) :  ORIGIN = 0x20000008, LENGTH = 0x20040000 - 0x20000008
  /* THICKET: origin was 0x20006000. The 24,576 B below it is reserved for
   * a SoftDevice's RAM. This firmware never calls sd_softdevice_enable and
   * links no BLE at all (zero sd_ble/bluefruit symbols in the image), so
   * that window is dormant. Verified on the board: the whole range was
   * written with an address-derived pattern and read back with zero
   * mismatches, then re-checked after 64 s of radio receive, USB traffic
   * and LXMF announces -- still zero. Recovers 24,568 B to .heap.
   * FLASH is deliberately untouched at 0x26000 so the bootloader still
   * finds the application exactly where it expects it. The bottom 8 B are
   * left to the MBR's parameter page. */
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM
  
  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"
