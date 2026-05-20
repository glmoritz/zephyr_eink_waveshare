#ifndef LLSS_SECTION_ATTRS_H_
#define LLSS_SECTION_ATTRS_H_

/*
 * Section placement macros.
 *
 * On ESP32 these expand to the original __attribute__((section(...))) so
 * RTC slow RAM (.rtc_noinit) and PSRAM (.ext_ram_noinit.*) placement is
 * preserved byte-for-byte. On CONFIG_ARCH_POSIX (native_sim) the section
 * directives degrade to no-ops; symbols land in plain BSS, which is fine
 * because the host process has no analogous power domains.
 */

#if defined(CONFIG_ARCH_POSIX)
#  define LLSS_SECTION_ATTR(name)
#else
#  define LLSS_SECTION_ATTR(name) __attribute__((section(name)))
#endif

#define LLSS_RTC_NOINIT             __attribute__((used)) LLSS_SECTION_ATTR(".rtc_noinit")
#define LLSS_EXT_RAM_NOINIT(suffix) LLSS_SECTION_ATTR(".ext_ram_noinit." suffix)

#endif /* LLSS_SECTION_ATTRS_H_ */
