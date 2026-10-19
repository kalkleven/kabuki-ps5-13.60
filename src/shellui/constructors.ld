/* The manual ELF loader enters elf_main directly, without the normal C++ startup. */
SECTIONS {
  .init_array : ALIGN(8) {
    PROVIDE_HIDDEN(overlay_init_begin = .);
    /* Prospero emits legacy .ctors; collect both, as the native app linker does. */
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP(*(.init_array .ctors))
    PROVIDE_HIDDEN(overlay_init_end = .);
  }
} INSERT BEFORE .data.rel.ro;
ASSERT(SIZEOF(.init_array) > 0, "Injected player overlay worker has no C++ constructors")
PROVIDE_HIDDEN(__eh_frame_start = ADDR(.eh_frame));
PROVIDE_HIDDEN(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE_HIDDEN(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE_HIDDEN(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
