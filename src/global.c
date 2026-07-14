/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static void mdbx_init(void);
static void mdbx_fini(void);

/*----------------------------------------------------------------------------*/
/* mdbx constructor/destructor */

#if IS_WINDOWS

#if MDBX_BUILD_SHARED_LIBRARY
#if MDBX_WITHOUT_MSVC_CRT && !defined(_DEBUG)
/* DEBUG/CHECKED builds still require MSVC's CRT for runtime checks.
 *
 * Define dll's entry point only for Release build when _DEBUG is not defined and
 * MDBX_WITHOUT_MSVC_CRT=ON. if the entry point isn't defined then MSVC's will
 * automatically use DllMainCRTStartup() from CRT library, which also
 * automatically call DllMain() from our mdbx.dll */
#pragma comment(linker, "/ENTRY:DllMain")

#if defined(_M_IX86) || defined(_X86_)
/*
 * The following two names are automatically created by the linker for any
 * image that has the safe exception table present.
 */
extern PVOID __safe_se_handler_table[]; /* base of safe handler entry table */
extern BYTE __safe_se_handler_count;    /* absolute symbol whose address is the count of table entries */

const __declspec(selectany) IMAGE_LOAD_CONFIG_DIRECTORY _load_config_used = {
    .SEHandlerTable = (SIZE_T)__safe_se_handler_table,
    .SEHandlerCount = (SIZE_T)&__safe_se_handler_count,
    .Size = sizeof(_load_config_used)};
#endif /* x86 */

#endif /* MDBX_WITHOUT_MSVC_CRT */

BOOL APIENTRY DllMain(HANDLE module, DWORD reason, LPVOID reserved)
#else
#if !MDBX_MANUAL_MODULE_HANDLER
static
#endif /* !MDBX_MANUAL_MODULE_HANDLER */
    void NTAPI mdbx_module_handler(PVOID module, DWORD reason, PVOID reserved)
#endif /* MDBX_BUILD_SHARED_LIBRARY */
{
  (void)reserved;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    windows_import();
    mdbx_init();
    break;
  case DLL_PROCESS_DETACH:
    mdbx_fini();
    break;

  case DLL_THREAD_ATTACH:
    break;
  case DLL_THREAD_DETACH:
    rthc_thread_dtor(module);
    break;
  }
#if MDBX_BUILD_SHARED_LIBRARY
  return TRUE;
#endif
}

#if !MDBX_BUILD_SHARED_LIBRARY && !MDBX_MANUAL_MODULE_HANDLER
/* *INDENT-OFF* */
/* clang-format off */
#if defined(_MSC_VER)
#  pragma const_seg(push)
#  pragma data_seg(push)

#  ifndef _M_IX86
     /* kick a linker to create the TLS directory if not already done */
#    pragma comment(linker, "/INCLUDE:_tls_used")
     /* Force some symbol references. */
#    pragma comment(linker, "/INCLUDE:mdbx_tls_anchor")
     /* specific const-segment for WIN64 */
#    pragma const_seg(".CRT$XLB")
     const
#  else
     /* kick a linker to create the TLS directory if not already done */
#    pragma comment(linker, "/INCLUDE:__tls_used")
     /* Force some symbol references. */
#    pragma comment(linker, "/INCLUDE:_mdbx_tls_anchor")
     /* specific data-segment for WIN32 */
#    pragma data_seg(".CRT$XLB")
#  endif

   __declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK mdbx_tls_anchor = mdbx_module_handler;
#  pragma data_seg(pop)
#  pragma const_seg(pop)

#elif defined(__GNUC__) || defined(__CODEGEARC__)
#  ifndef _M_IX86
     const
#  endif
   PIMAGE_TLS_CALLBACK mdbx_tls_anchor __attribute__((__section__(".CRT$XLB"), used)) = mdbx_module_handler;
#else
#  error FIXME
#endif
/* *INDENT-ON* */
/* clang-format on */
#endif /* !MDBX_BUILD_SHARED_LIBRARY && !MDBX_MANUAL_MODULE_HANDLER */

#else

#if defined(__linux__) || defined(__gnu_linux__)
#include <sys/utsname.h>

MDBX_EXCLUDE_FOR_GPROF
__cold static uint8_t probe_for_WSL(const char *tag) {
  const char *const WSL = strstr(tag, "WSL");
  if (WSL && WSL[3] >= '2' && WSL[3] <= '9')
    return WSL[3] - '0';
  const char *const wsl = strstr(tag, "wsl");
  if (wsl && wsl[3] >= '2' && wsl[3] <= '9')
    return wsl[3] - '0';
  if (WSL || wsl || strcasestr(tag, "Microsoft"))
    /* Expecting no new kernel within WSL1, either it will explicitly
     * marked by an appropriate WSL-version hint. */
    return (globals.linux_kernel_version < /* 4.19.x */ 0x04130000) ? 1 : 2;
  return 0;
}
#endif /* Linux */

#ifdef ENABLE_GPROF
extern void _mcleanup(void);
extern void monstartup(unsigned long, unsigned long);
extern void _init(void);
extern void _fini(void);
extern void __gmon_start__(void) __attribute__((__weak__));
#endif /* ENABLE_GPROF */

MDBX_EXCLUDE_FOR_GPROF
__cold static __attribute__((__constructor__)) void mdbx_global_constructor(void) {
#ifdef ENABLE_GPROF
  if (!&__gmon_start__)
    monstartup((uintptr_t)&_init, (uintptr_t)&_fini);
#endif /* ENABLE_GPROF */

#if defined(__linux__) || defined(__gnu_linux__)
  struct utsname buffer;
  if (uname(&buffer) == 0) {
    int i = 0;
    char *p = buffer.release;
    while (*p && i < 4) {
      if (*p >= '0' && *p <= '9') {
        long number = strtol(p, &p, 10);
        if (number > 0) {
          if (number > 255)
            number = 255;
          globals.linux_kernel_version += number << (24 - i * 8);
        }
        ++i;
      } else {
        ++p;
      }
    }
    /* "Official" way of detecting WSL1 but not WSL2
     * https://github.com/Microsoft/WSL/issues/423#issuecomment-221627364
     *
     * WARNING: False negative detection of WSL1 will result in DATA LOSS!
     * So, the REQUIREMENTS for this code:
     *  1. MUST detect WSL1 without false-negatives.
     *  2. DESIRABLE detect WSL2 but without the risk of violating the first. */
    globals.running_on_WSL1 =
        probe_for_WSL(buffer.version) == 1 || probe_for_WSL(buffer.sysname) == 1 || probe_for_WSL(buffer.release) == 1;
  }
#endif /* Linux */

  mdbx_init();
}

MDBX_EXCLUDE_FOR_GPROF
__cold static __attribute__((__destructor__)) void mdbx_global_destructor(void) {
  mdbx_fini();
#ifdef ENABLE_GPROF
  if (!&__gmon_start__)
    _mcleanup();
#endif /* ENABLE_GPROF */
}

#endif /* ! Windows */

/******************************************************************************/

struct libmdbx_globals globals;

static bool getenv_bool(const char *name, bool default_value) {
  const char *value = osal_getenv_singlethreaded(name, false);
  if (value) {
    if (*value == 0 /* implied ON */)
      return true;
    if (strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "1") == 0)
      return true;
    if (strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "0") == 0)
      return false;
  }
  return default_value;
}

__cold static size_t assume_ram_pages(void) {
  intptr_t total_ram_pages, avail_ram_pages;
  int err = mdbx_get_sysraminfo(nullptr, &total_ram_pages, &avail_ram_pages);
  if (unlikely(err != MDBX_SUCCESS)) {
    /* the 32-bit limit is good enough for fallback */
    total_ram_pages = MAX_MAPSIZE32 / 3 >> globals.sys_pagesize_ln2;
    avail_ram_pages = total_ram_pages / 2;
  }

  size_t result = (size_t)(total_ram_pages + avail_ram_pages) / 2;
  if (RUNNING_ON_ASAN)
    result = avail_ram_pages >> 1;
  if (mdbx_running_on_Valgrind())
    result = avail_ram_pages >> 4;

  return result;
}

__cold static size_t mmap_limit(void) {
  STATIC_ASSERT(MAX_MAPSIZE < INTPTR_MAX);
  size_t limit = MAX_MAPSIZE;

  const uint64_t asan_limit = UINT64_C(16383) * GIGABYTE;
  if (RUNNING_ON_ASAN && limit > asan_limit)
    limit = (size_t)asan_limit;

  const size_t valgrind_limit = (MDBX_WORDBITS < 64) ? 512 * MEGABYTE : (size_t)(32 * GIGABYTE);
  if (mdbx_running_on_Valgrind() && limit > valgrind_limit)
    limit = valgrind_limit;

  if (RUNNING_ON_ASAN || mdbx_running_on_Valgrind()) {
    ASSERT(globals.assume_ram_pages > 0);
    limit = min_unsigned(limit, globals.assume_ram_pages << globals.sys_pagesize_ln2);
  }
  return limit;
}

__cold static size_t reasonable_db_maxsize(void) {
  ASSERT(globals.assume_ram_pages > 0 && globals.sys_pagesize_ln2 && globals.mmap_limit);
  /* Suggesting should not be more than golden ratio of the size of RAM. */
  size_t result = (globals.assume_ram_pages * 207 >> 7) << globals.sys_pagesize_ln2;
  if (result > globals.mmap_limit / 2)
    return globals.mmap_limit / 2;

  /* Round to the nearest human-readable granulation. */
  for (size_t unit = MEGABYTE; unit; unit <<= 5) {
    const size_t floor = floor_powerof2(result, unit);
    const size_t ceil = ceil_powerof2(result, unit);
    const size_t threshold = (size_t)result >> 4;
    const bool down = result - floor < ceil - result || ceil > globals.mmap_limit / 4;
    if (threshold < (down ? result - floor : ceil - result))
      break;
    result = down ? floor : ceil;
  }
  ASSERT(result <= globals.mmap_limit);
  return result;
}

__cold static void mdbx_init(void) {
#ifdef ENABLE_MEMCHECK
  globals.running_on_Valgrind = RUNNING_ON_VALGRIND;
#endif /* ENABLE_MEMCHECK */
  globals.runtime_flags = (getenv_bool("MDBX_DBG_ASSERT", (MDBX_CHECKING) > 1) ? MDBX_DBG_ASSERT : 0) |
                          (getenv_bool("MDBX_DBG_AUDIT", (MDBX_CHECKING) > 2) ? MDBX_DBG_AUDIT : 0) |
                          (getenv_bool("MDBX_DBG_JITTER", false) ? MDBX_DBG_JITTER : 0) |
                          (getenv_bool("MDBX_DBG_DUMP", false) ? MDBX_DBG_DUMP : 0) |
                          (getenv_bool("MDBX_DBG_LEGACY_MULTIOPEN", false) ? MDBX_DBG_LEGACY_MULTIOPEN : 0) |
                          (getenv_bool("MDBX_DBG_LEGACY_OVERLAP", false) ? MDBX_DBG_LEGACY_OVERLAP : 0) |
                          (getenv_bool("MDBX_DBG_DONT_UPGRADE", false) ? MDBX_DBG_DONT_UPGRADE : 0);
  globals.loglevel = MDBX_LOG_NOTICE;
  ENSURE(osal_fastmutex_init(&globals.debug_lock) == 0);
  osal_ctor();
  ASSERT(globals.sys_pagesize > 0 && (globals.sys_pagesize & (globals.sys_pagesize - 1)) == 0);
  rthc_ctor();
#if MDBX_CHECKING > 0
  ENSURE(troika_verify_fsm());
  ENSURE(pv2pages_verify());
#endif /* MDBX_CHECKING > 0 */

  globals.assume_ram_pages = assume_ram_pages();
  globals.mmap_limit = mmap_limit();
  globals.reasonable_db_maxsize = reasonable_db_maxsize();
}

MDBX_EXCLUDE_FOR_GPROF
__cold static void mdbx_fini(void) {
  const mdbx_pid_t current_pid = osal_getpid();
  TRACE(">> pid %zd", (size_t)current_pid);
  rthc_dtor(current_pid);
  osal_dtor();
  TRACE("<< pid %zd\n", (size_t)current_pid);
  ENSURE(osal_fastmutex_destroy(&globals.debug_lock) == 0);
}

/******************************************************************************/

/* *INDENT-OFF* */
/* clang-format off */

__dll_export
#ifdef __attribute_used__
    __attribute_used__
#elif defined(__GNUC__) || __has_attribute(__used__)
    __attribute__((__used__))
#endif
#ifdef __attribute_externally_visible__
        __attribute_externally_visible__
#elif (defined(__GNUC__) && !defined(__clang__)) ||                            \
    __has_attribute(__externally_visible__)
    __attribute__((__externally_visible__))
#endif
    const struct MDBX_build_info mdbx_build = {
#ifdef MDBX_BUILD_TIMESTAMP
    MDBX_BUILD_TIMESTAMP
#else
    "\"" __DATE__ " " __TIME__ "\""
#endif /* MDBX_BUILD_TIMESTAMP */

    ,
#ifdef MDBX_BUILD_TARGET
    MDBX_BUILD_TARGET
#else
  #if defined(__ANDROID_API__)
    "Android" MDBX_STRINGIFY(__ANDROID_API__)
  #elif defined(__OHOS__)
    "Harmony OS"
  #elif defined(__linux__) || defined(__gnu_linux__)
    "Linux"
  #elif defined(EMSCRIPTEN) || defined(__EMSCRIPTEN__)
    "webassembly"
  #elif defined(__CYGWIN__)
    "CYGWIN"
  #elif defined(_WIN64) || defined(_WIN32) || defined(__TOS_WIN__) \
      || defined(__WINDOWS__)
    "Windows"
  #elif defined(__APPLE__)
    #if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) \
      || (defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR)
      "iOS"
    #else
      "MacOS"
    #endif
  #elif defined(__FreeBSD__)
    "FreeBSD"
  #elif defined(__DragonFly__)
    "DragonFlyBSD"
  #elif defined(__NetBSD__)
    "NetBSD"
  #elif defined(__OpenBSD__)
    "OpenBSD"
  #elif defined(__bsdi__)
    "UnixBSDI"
  #elif defined(__MACH__)
    "MACH"
  #elif (defined(_HPUX_SOURCE) || defined(__hpux) || defined(__HP_aCC))
    "HPUX"
  #elif defined(_AIX)
    "AIX"
  #elif defined(__sun) && defined(__SVR4)
    "Solaris"
  #elif defined(__BSD__) || defined(BSD)
    "UnixBSD"
  #elif defined(__unix__) || defined(UNIX) || defined(__unix) \
      || defined(__UNIX) || defined(__UNIX__)
    "UNIX"
  #elif defined(_POSIX_VERSION)
    "POSIX" MDBX_STRINGIFY(_POSIX_VERSION)
  #else
    "UnknownOS"
  #endif /* Target OS */

    "-"

  #if defined(__e2k__) || defined(__elbrus__)
    "Elbrus"
  #elif defined(__amd64__)
    "AMD64"
  #elif defined(__ia32__)
    "IA32"
  #elif defined(__e2k__) || defined(__elbrus__)
    "Elbrus"
  #elif defined(__alpha__) || defined(__alpha) || defined(_M_ALPHA)
    "Alpha"
  #elif defined(__aarch64__) || defined(_M_ARM64)
    "ARM64"
  #elif defined(__arm__) || defined(__thumb__) || defined(__TARGET_ARCH_ARM) \
      || defined(__TARGET_ARCH_THUMB) || defined(_ARM) || defined(_M_ARM) \
      || defined(_M_ARMT) || defined(__arm)
    "ARM"
  #elif defined(__mips64) || defined(__mips64__) || (defined(__mips) && (__mips >= 64))
    "MIPS64"
  #elif defined(__mips__) || defined(__mips) || defined(_R4000) || defined(__MIPS__)
    "MIPS"
  #elif defined(__hppa64__) || defined(__HPPA64__) || defined(__hppa64)
    "PARISC64"
  #elif defined(__hppa__) || defined(__HPPA__) || defined(__hppa)
    "PARISC"
  #elif defined(__ia64__) || defined(__ia64) || defined(_IA64) \
      || defined(__IA64__) || defined(_M_IA64) || defined(__itanium__)
    "Itanium"
  #elif defined(__powerpc64__) || defined(__ppc64__) || defined(__ppc64) \
      || defined(__powerpc64) || defined(_ARCH_PPC64)
    "PowerPC64"
  #elif defined(__powerpc__) || defined(__ppc__) || defined(__powerpc) \
      || defined(__ppc) || defined(_ARCH_PPC) || defined(__PPC__) || defined(__POWERPC__)
    "PowerPC"
  #elif defined(__sparc64__) || defined(__sparc64)
    "SPARC64"
  #elif defined(__sparc__) || defined(__sparc)
    "SPARC"
  #elif defined(__s390__) || defined(__s390) || defined(__zarch__) || defined(__zarch)
    "S390"
  #elif defined(__riscv) || defined(__riscv__) || defined(__RISCV) || defined(__RISCV__)
    "RISC-V (стеклянные бусы)"
  #else
    "UnknownARCH"
  #endif
#endif /* MDBX_BUILD_TARGET */

#ifdef MDBX_BUILD_TYPE
# if defined(_MSC_VER)
#   pragma message("Configuration-depended MDBX_BUILD_TYPE: " MDBX_BUILD_TYPE)
# endif
    "-" MDBX_BUILD_TYPE
#endif /* MDBX_BUILD_TYPE */
    ,
    "DEBUG=" MDBX_STRINGIFY(MDBX_DEBUG)
    " CHECKING=" MDBX_STRINGIFY(MDBX_CHECKING)
#ifdef ENABLE_GPROF
    " ENABLE_GPROF"
#endif /* ENABLE_GPROF */
    " WORDBITS=" MDBX_STRINGIFY(MDBX_WORDBITS)
    " BYTE_ORDER="
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    "LE"
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    "BE"
#else
    #error "FIXME: Unsupported byte order"
#endif /* __BYTE_ORDER__ */
    " BIGFOOT=" MDBX_STRINGIFY(MDBX_ENABLE_BIGFOOT)
    " ENV_CHECKPID=" MDBX_ENV_CHECKPID_CONFIG
    " TXN_CHECKOWNER=" MDBX_TXN_CHECKOWNER_CONFIG
    " 64BIT_ATOMIC=" MDBX_64BIT_ATOMIC_CONFIG
    " 64BIT_CAS=" MDBX_64BIT_CAS_CONFIG
    " TRUST_RTC=" MDBX_TRUST_RTC_CONFIG
    " AVOID_MSYNC=" MDBX_STRINGIFY(MDBX_AVOID_MSYNC)
    " REFUND=" MDBX_STRINGIFY(MDBX_ENABLE_REFUND)
    " MINCORE=" MDBX_STRINGIFY(MDBX_USE_MINCORE)
    " PGOP_STAT=" MDBX_STRINGIFY(MDBX_ENABLE_PGOP_STAT)
    " PROFGC=" MDBX_STRINGIFY(MDBX_ENABLE_PROFGC)
    " PGET_STAT=" MDBX_STRINGIFY(MDBX_ENABLE_PGET_STAT)
#if MDBX_DISABLE_VALIDATION
    " DISABLE_VALIDATION=YES"
#endif /* MDBX_DISABLE_VALIDATION */
#ifdef __SANITIZE_ADDRESS__
    " SANITIZE_ADDRESS=YES"
#endif /* __SANITIZE_ADDRESS__ */
#ifdef ENABLE_MEMCHECK
    " MEMCHECK=YES"
#endif /* ENABLE_MEMCHECK */
#ifdef ENABLE_SYSTEMTAP
    " SYSTEMTAP=YES"
#elif defined(ENABLE_DTRACE)
    " DTRACE=YES"
#endif /* ENABLE_DTRACE || ENABLE_SYSTEMTAP */
#ifdef _GNU_SOURCE
    " _GNU_SOURCE=YES"
#else
    " _GNU_SOURCE=NO"
#endif /* _GNU_SOURCE */
#ifdef __APPLE__
    " MDBX_APPLE_SPEED_INSTEADOF_DURABILITY=" MDBX_STRINGIFY(MDBX_APPLE_SPEED_INSTEADOF_DURABILITY)
#endif /* MacOS */
#if IS_WINDOWS
    " WITHOUT_MSVC_CRT=" MDBX_STRINGIFY(MDBX_WITHOUT_MSVC_CRT)
    " NATIVE_SEH=" MDBX_STRINGIFY(MDBX_NATIVE_SEH)
    " BUILD_SHARED_LIBRARY=" MDBX_STRINGIFY(MDBX_BUILD_SHARED_LIBRARY)
#if !MDBX_BUILD_SHARED_LIBRARY
    " MANUAL_MODULE_HANDLER=" MDBX_STRINGIFY(MDBX_MANUAL_MODULE_HANDLER)
#endif
    " WINVER=" MDBX_STRINGIFY(WINVER)
#else /* Windows */
    " LOCKING=" MDBX_LOCKING_CONFIG
    " OFDLOCKS=" MDBX_USE_OFDLOCKS_CONFIG
    " FALLOCATE=" MDBX_USE_FALLOCATE_CONFIG
#endif /* !Windows */
    " CACHELINE_SIZE=" MDBX_STRINGIFY(MDBX_CACHELINE_SIZE)
    " CPU_WRITEBACK_INCOHERENT=" MDBX_STRINGIFY(MDBX_CPU_WRITEBACK_INCOHERENT)
    " MMAP_INCOHERENT_CPU_CACHE=" MDBX_STRINGIFY(MDBX_MMAP_INCOHERENT_CPU_CACHE)
    " MMAP_INCOHERENT_FILE_WRITE=" MDBX_STRINGIFY(MDBX_MMAP_INCOHERENT_FILE_WRITE)
    " UNALIGNED_OK=" MDBX_STRINGIFY(MDBX_UNALIGNED_OK)
#if MDBX_PNL_ASCENDING
    " PNL_ASCENDING=" MDBX_STRINGIFY(MDBX_PNL_ASCENDING)
#endif /* MDBX_PNL_ASCENDING */
    ,
#ifdef MDBX_BUILD_COMPILER
    MDBX_BUILD_COMPILER
#else
  #ifdef __INTEL_COMPILER
    "Intel C/C++ " MDBX_STRINGIFY(__INTEL_COMPILER)
  #elif defined(__apple_build_version__)
    "Apple clang " MDBX_STRINGIFY(__apple_build_version__)
  #elif defined(__ibmxl__)
    "IBM clang C " MDBX_STRINGIFY(__ibmxl_version__) "." MDBX_STRINGIFY(__ibmxl_release__)
    "." MDBX_STRINGIFY(__ibmxl_modification__) "." MDBX_STRINGIFY(__ibmxl_ptf_fix_level__)
  #elif defined(__clang__)
    "clang " MDBX_STRINGIFY(__clang_version__)
  #elif defined(__MINGW64__)
    "MINGW-64 " MDBX_STRINGIFY(__MINGW64_MAJOR_VERSION) "." MDBX_STRINGIFY(__MINGW64_MINOR_VERSION)
  #elif defined(__MINGW32__)
    "MINGW-32 " MDBX_STRINGIFY(__MINGW32_MAJOR_VERSION) "." MDBX_STRINGIFY(__MINGW32_MINOR_VERSION)
  #elif defined(__MINGW__)
    "MINGW " MDBX_STRINGIFY(__MINGW_MAJOR_VERSION) "." MDBX_STRINGIFY(__MINGW_MINOR_VERSION)
  #elif defined(__IBMC__)
    "IBM C " MDBX_STRINGIFY(__IBMC__)
  #elif defined(__GNUC__)
    "GNU C/C++ "
    #ifdef __VERSION__
      __VERSION__
    #else
      MDBX_STRINGIFY(__GNUC__) "." MDBX_STRINGIFY(__GNUC_MINOR__) "." MDBX_STRINGIFY(__GNUC_PATCHLEVEL__)
    #endif
  #elif defined(_MSC_VER)
    "MSVC " MDBX_STRINGIFY(_MSC_FULL_VER) "-" MDBX_STRINGIFY(_MSC_BUILD)
  #else
    "Unknown compiler"
  #endif
#endif /* MDBX_BUILD_COMPILER */
    ,
#ifdef MDBX_BUILD_FLAGS_CONFIG
    MDBX_BUILD_FLAGS_CONFIG
#endif /* MDBX_BUILD_FLAGS_CONFIG */
#if defined(MDBX_BUILD_FLAGS_CONFIG) && defined(MDBX_BUILD_FLAGS)
    " "
#endif
#ifdef MDBX_BUILD_FLAGS
    MDBX_BUILD_FLAGS
#endif /* MDBX_BUILD_FLAGS */
#if !(defined(MDBX_BUILD_FLAGS_CONFIG) || defined(MDBX_BUILD_FLAGS))
    "undefined (please use correct build script)"
#ifdef _MSC_VER
#pragma message("warning: Build flags undefined. Please use correct build script")
#else
#warning "Build flags undefined. Please use correct build script"
#endif // _MSC_VER
#endif
  , MDBX_BUILD_METADATA
};

#ifdef __SANITIZE_ADDRESS__
#if !defined(_MSC_VER) || __has_attribute(weak)
LIBMDBX_API __attribute__((__weak__))
#endif
const char *__asan_default_options(void) {
  return "symbolize=1:allow_addr2line=1:"
#if MDBX_DEBUG > 0
         "debug=1:"
         "verbosity=2:"
#endif /* MDBX_DEBUG */
         "log_threads=1:"
         "report_globals=1:"
         "replace_str=1:replace_intrin=1:"
         "malloc_context_size=9:"
#if !defined(__APPLE__)
         "detect_leaks=1:"
#endif
         "check_printf=1:"
         "detect_deadlocks=1:"
#ifndef LTO_ENABLED
         "check_initialization_order=1:"
#endif
         "detect_stack_use_after_return=1:"
         "intercept_tls_get_addr=1:"
         "decorate_proc_maps=1:"
         "abort_on_error=1";
}
#endif /* __SANITIZE_ADDRESS__ */

/* *INDENT-ON* */
/* clang-format on */
