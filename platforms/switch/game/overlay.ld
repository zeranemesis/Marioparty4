/*
 * Relocatable link of one overlay (ld -r, see SwitchGame.cmake): gather its
 * writable initialised data and its bss into one section each, so the
 * executable can find them (__start_/__stop_ symbols) and reset them whenever
 * the game links the overlay again (include/port/static_overlays.h).
 * Read-only data, including .data.rel.ro, is left alone.
 */
SECTIONS
{
    pb_ovl_data : { *(.data .data.rel .data.rel.local .data.rel.local.*) }
    pb_ovl_bss : { *(.bss .bss.* COMMON) }
}
