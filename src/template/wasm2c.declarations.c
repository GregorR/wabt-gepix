
// Computes a pointer to an object of the given size in a little-endian memory.
//
// On a little-endian host, this is just &mem->data[addr] - the object's size is
// unused. On a big-endian host, it's &mem->data[mem->size - addr - n], where n
// is the object's size.
//
// Note that mem may be evaluated multiple times.
//
// Parameters:
// mem - The memory.
// addr - The address.
// n - The size of the object.
//
// Result:
// A pointer for an object of size n.
#if WABT_BIG_ENDIAN
#define MEM_ADDR(mem, addr, n) &(mem)->data[(mem)->size - (addr) - (n)]
#else
#define MEM_ADDR(mem, addr, n) &(mem)->data[addr]
#endif

#ifndef WASM_RT_USE_SEGUE
// Memory functions can use the segue optimization if allowed. The segue
// optimization uses x86 segments to point to a linear memory. We use this
// optimization when:
//
// (1) Segue is allowed using WASM_RT_ALLOW_SEGUE
// (2) on x86_64 without WABT_BIG_ENDIAN enabled
// (3) the Wasm module uses a single unshared imported or exported memory
// (4) the compiler supports: intrinsics for (rd|wr)gsbase, "address namespaces"
//     for accessing pointers, and supports memcpy on pointers with custom
//     "address namespaces". GCC does not support the memcpy requirement, so
//     this leaves only clang for now.
// (5) The OS provides a way to query if (rd|wr)gsbase is allowed by the kernel
// or the implementation has to use a syscall for this.
// (6) The OS doesn't replace the segment register on context switch which
//     eliminates windows for now
//
// While more OS can be supported in the future, we only support linux for now
#if WASM_RT_ALLOW_SEGUE && !WABT_BIG_ENDIAN &&                               \
    (defined(__x86_64__) || defined(_M_X64)) && IS_SINGLE_UNSHARED_MEMORY && \
    __clang__ && __has_builtin(__builtin_ia32_wrgsbase64) &&                 \
    !defined(_WIN32) && defined(__linux__)
#define WASM_RT_USE_SEGUE 1
#else
#define WASM_RT_USE_SEGUE 0
#endif
#endif

#if WASM_RT_USE_SEGUE
// POSIX uses FS for TLS, GS is free
static inline void* wasm_rt_segue_read_base() {
  if (wasm_rt_fsgsbase_inst_supported) {
    return (void*)__builtin_ia32_rdgsbase64();
  } else {
    return wasm_rt_syscall_get_segue_base();
  }
}
static inline void wasm_rt_segue_write_base(void* base) {
  if (wasm_rt_fsgsbase_inst_supported) {
    __builtin_ia32_wrgsbase64((uintptr_t)base);
  } else {
    wasm_rt_syscall_set_segue_base(base);
  }
}
#define MEM_ADDR_MEMOP(mem, addr, n) ((uint8_t __seg_gs*)(uintptr_t)addr)
#else
#define MEM_ADDR_MEMOP(mem, addr, n) MEM_ADDR(mem, addr, n)
#endif

#define TRAP(x) (wasm_rt_trap(WASM_RT_TRAP_##x), 0)

#if WASM_RT_STACK_DEPTH_COUNT
#define FUNC_PROLOGUE                                            \
  if (++wasm_rt_call_stack_depth > WASM_RT_MAX_CALL_STACK_DEPTH) \
    TRAP(EXHAUSTION);

#define FUNC_EPILOGUE --wasm_rt_call_stack_depth
#else
#define FUNC_PROLOGUE

#define FUNC_EPILOGUE
#endif

#define UNREACHABLE TRAP(UNREACHABLE)

static inline bool func_types_eq(const wasm_rt_func_type_t a,
                                 const wasm_rt_func_type_t b) {
  return (a == b) || LIKELY(a && b && !memcmp(a, b, 32));
}

#if WASM_RT_NONCONFORMING_MEMCHECK_NONE
#define CHECK_CALL_INDIRECT(table, ft, x) (void) 0
#else
#define CHECK_CALL_INDIRECT(table, ft, x)                \
  (LIKELY((x) < table.size && table.data[x].func &&      \
          func_types_eq(ft, table.data[x].func_type)) || \
   TRAP(CALL_INDIRECT))
#endif

#define DO_CALL_INDIRECT(table, t, x, ...) \
    GGT_CALL(((t)table.data[x].func), (__VA_ARGS__))

#define CALL_INDIRECT(table, t, ft, x, ...) do { \
  CHECK_CALL_INDIRECT(table, ft, x);             \
  DO_CALL_INDIRECT(table, t, x, __VA_ARGS__);    \
} while (0)

#if WASM_RT_NONCONFORMING_MEMCHECK_NONE
#define RANGE_CHECK(mem, offset, len) (void) 0
#else
#ifdef SUPPORT_MEMORY64
#define RANGE_CHECK(mem, offset, len)              \
  do {                                             \
    uint64_t res;                                  \
    if (__builtin_add_overflow(offset, len, &res)) \
      TRAP(OOB);                                   \
    if (UNLIKELY(res > mem->size))                 \
      TRAP(OOB);                                   \
  } while (0);
#else
#define RANGE_CHECK(mem, offset, len)               \
  if (UNLIKELY(offset + (uint64_t)len > mem->size)) \
    TRAP(OOB);
#endif
#endif

#if WASM_RT_USE_SEGUE && WASM_RT_SANITY_CHECKS
#include <stdio.h>
#define WASM_RT_CHECK_BASE(mem)                                               \
  if (((uintptr_t)((mem)->data)) != ((uintptr_t)wasm_rt_segue_read_base())) { \
    puts("Segment register mismatch\n");                                      \
    abort();                                                                  \
  }
#else
#define WASM_RT_CHECK_BASE(mem)
#endif

#if WASM_RT_MEMCHECK_GUARD_PAGES
#define MEMCHECK(mem, a, t) WASM_RT_CHECK_BASE(mem);
#elif WASM_RT_MEMCHECK_BOUNDS_CHECK
#define MEMCHECK(mem, a, t) \
  WASM_RT_CHECK_BASE(mem);  \
  RANGE_CHECK(mem, a, sizeof(t))
#else
#define MEMCHECK(mem, a, t) (void) 0
#endif

#ifdef __GNUC__
#define FORCE_READ_INT(var) __asm__("" ::"r"(var));
// Clang on Mips requires "f" constraints on floats
// See https://github.com/llvm/llvm-project/issues/64241
#if defined(__clang__) && \
    (defined(mips) || defined(__mips__) || defined(__mips))
#define FORCE_READ_FLOAT(var) __asm__("" ::"f"(var));
#else
#define FORCE_READ_FLOAT(var) __asm__("" ::"r"(var));
#endif
#else
#define FORCE_READ_INT(var)
#define FORCE_READ_FLOAT(var)
#endif

static inline void load_data(void* dest, const void* src, size_t n) {
  if (!n) {
    return;
  }
  wasm_rt_memcpy(dest, src, n);
#if WABT_BIG_ENDIAN
  u8* dest_chars = dest;
  for (size_t i = 0; i < (n >> 1); i++) {
    u8 cursor = dest_chars[i];
    dest_chars[i] = dest_chars[n - i - 1];
    dest_chars[n - i - 1] = cursor;
  }
#endif
}

#define LOAD_DATA(m, o, i, s)            \
  do {                                   \
    RANGE_CHECK((&m), o, s);             \
    load_data(MEM_ADDR(&m, o, s), i, s); \
  } while (0)

#if WASM_RT_NONCONFORMING_MEMCHECK_NONE
#define DEFINE_LOAD(name, t1, t2, t3, force_read)                     \
  static inline t3 name(wasm_rt_memory_t* mem, size_t addr) {         \
    return (t3) (t2) *((t1 *) MEM_ADDR_MEMOP(mem, addr, sizeof(t1))); \
  }

#define DEFINE_STORE(name, t1, t2)                                        \
  static inline void name(wasm_rt_memory_t* mem, size_t addr, t2 value) { \
    *((t1 *) MEM_ADDR_MEMOP(mem, addr, sizeof(t1))) = (t1) value;         \
  }

#else
#define DEFINE_LOAD(name, t1, t2, t3, force_read)                  \
  static inline t3 name(wasm_rt_memory_t* mem, u64 addr) {         \
    t1 result;                                                     \
    MEMCHECK(mem, addr, t1);                                       \
    wasm_rt_memcpy(&result, MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), \
                   sizeof(t1));                                    \
    force_read(result);                                            \
    return (t3)(t2)result;                                         \
  }

#define DEFINE_STORE(name, t1, t2)                                     \
  static inline void name(wasm_rt_memory_t* mem, u64 addr, t2 value) { \
    t1 wrapped = (t1)value;                                            \
    MEMCHECK(mem, addr, t1);                                           \
    wasm_rt_memcpy(MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), &wrapped,    \
                   sizeof(t1));                                        \
  }

#endif

DEFINE_LOAD(i32_load, u32, u32, u32, FORCE_READ_INT)
DEFINE_LOAD(i64_load, u64, u64, u64, FORCE_READ_INT)
DEFINE_LOAD(f32_load, f32, f32, f32, FORCE_READ_FLOAT)
DEFINE_LOAD(f64_load, f64, f64, f64, FORCE_READ_FLOAT)
DEFINE_LOAD(i32_load8_s, s8, s32, u32, FORCE_READ_INT)
DEFINE_LOAD(i64_load8_s, s8, s64, u64, FORCE_READ_INT)
DEFINE_LOAD(i32_load8_u, u8, u32, u32, FORCE_READ_INT)
DEFINE_LOAD(i64_load8_u, u8, u64, u64, FORCE_READ_INT)
DEFINE_LOAD(i32_load16_s, s16, s32, u32, FORCE_READ_INT)
DEFINE_LOAD(i64_load16_s, s16, s64, u64, FORCE_READ_INT)
DEFINE_LOAD(i32_load16_u, u16, u32, u32, FORCE_READ_INT)
DEFINE_LOAD(i64_load16_u, u16, u64, u64, FORCE_READ_INT)
DEFINE_LOAD(i64_load32_s, s32, s64, u64, FORCE_READ_INT)
DEFINE_LOAD(i64_load32_u, u32, u64, u64, FORCE_READ_INT)
DEFINE_STORE(i32_store, u32, u32)
DEFINE_STORE(i64_store, u64, u64)
DEFINE_STORE(f32_store, f32, f32)
DEFINE_STORE(f64_store, f64, f64)
DEFINE_STORE(i32_store8, u8, u32)
DEFINE_STORE(i32_store16, u16, u32)
DEFINE_STORE(i64_store8, u8, u64)
DEFINE_STORE(i64_store16, u16, u64)
DEFINE_STORE(i64_store32, u32, u64)

#if defined(_MSC_VER)

// Adapted from
// https://github.com/nemequ/portable-snippets/blob/master/builtin/builtin.h

static inline int I64_CLZ(unsigned long long v) {
  unsigned long r = 0;
#if defined(_M_AMD64) || defined(_M_ARM)
  if (_BitScanReverse64(&r, v)) {
    return 63 - r;
  }
#else
  if (_BitScanReverse(&r, (unsigned long)(v >> 32))) {
    return 31 - r;
  } else if (_BitScanReverse(&r, (unsigned long)v)) {
    return 63 - r;
  }
#endif
  return 64;
}

static inline int I32_CLZ(unsigned long v) {
  unsigned long r = 0;
  if (_BitScanReverse(&r, v)) {
    return 31 - r;
  }
  return 32;
}

static inline int I64_CTZ(unsigned long long v) {
  if (!v) {
    return 64;
  }
  unsigned long r = 0;
#if defined(_M_AMD64) || defined(_M_ARM)
  _BitScanForward64(&r, v);
  return (int)r;
#else
  if (_BitScanForward(&r, (unsigned int)(v))) {
    return (int)(r);
  }

  _BitScanForward(&r, (unsigned int)(v >> 32));
  return (int)(r + 32);
#endif
}

static inline int I32_CTZ(unsigned long v) {
  if (!v) {
    return 32;
  }
  unsigned long r = 0;
  _BitScanForward(&r, v);
  return (int)r;
}

#undef POPCOUNT_DEFINE_PORTABLE

#elif defined(__GNUC__)

#define I32_CLZ(x) ((x) ? __builtin_clz(x) : 32)
#define I64_CLZ(x) ((x) ? __builtin_clzll(x) : 64)
#define I32_CTZ(x) ((x) ? __builtin_ctz(x) : 32)
#define I64_CTZ(x) ((x) ? __builtin_ctzll(x) : 64)

#else

#define IX_CTZ(name, type, bits) \
static int name ## _CTZ(type x) { \
  int i; \
  for (i = 0; i < (bits); i++) { \
    if (x & 1) break; \
    x >>= 1; \
  } \
  return i; \
}

#define REV(name, type, bits) \
static type name ## _REV(type x) { \
  type r = 0; \
  int i; \
  for (i = 0; i < (bits); i++) { \
    r <<= 1; \
    if (x & 1) r |= 1; \
    i >>= 1; \
  } \
  return r; \
}

#define IX_CLZ(name, type) \
static int name ## _CLZ(type x) { \
  return name ## _CTZ(name ## _REV(x)); \
}

IX_CTZ(I32, uint32_t, 32)
IX_CTZ(I64, uint64_t, 64)
REV(I32, uint32_t, 32)
REV(I64, uint64_t, 64)
IX_CLZ(I32, uint32_t)
IX_CLZ(I64, uint64_t)

#undef IX_CTZ
#undef REV
#undef IX_CLZ

#endif

#if defined(__GNUC__)
#define I32_POPCNT(x) (__builtin_popcount(x))
#define I64_POPCNT(x) (__builtin_popcountll(x))

#else
#define POPCOUNT_DEFINE_PORTABLE(f_n, T)                            \
  static inline u32 f_n(T x) {                                      \
    x = x - ((x >> 1) & (T) ~(T)0 / 3);                             \
    x = (x & (T) ~(T)0 / 15 * 3) + ((x >> 2) & (T) ~(T)0 / 15 * 3); \
    x = (x + (x >> 4)) & (T) ~(T)0 / 255 * 15;                      \
    return (T)(x * ((T) ~(T)0 / 255)) >> (sizeof(T) - 1) * 8;       \
  }

POPCOUNT_DEFINE_PORTABLE(I32_POPCNT, u32)
POPCOUNT_DEFINE_PORTABLE(I64_POPCNT, u64)

#endif

#define DIV_S(ut, min, x, y)                                      \
  ((UNLIKELY((y) == 0))                                           \
       ? TRAP(DIV_BY_ZERO)                                        \
       : (UNLIKELY((x) == min && (y) == -1)) ? TRAP(INT_OVERFLOW) \
                                             : (ut)((x) / (y)))

#define REM_S(ut, min, x, y) \
  ((UNLIKELY((y) == 0))      \
       ? TRAP(DIV_BY_ZERO)   \
       : (UNLIKELY((x) == min && (y) == -1)) ? 0 : (ut)((x) % (y)))

#define I32_DIV_S(x, y) DIV_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_DIV_S(x, y) DIV_S(u64, INT64_MIN, (s64)x, (s64)y)
#define I32_REM_S(x, y) REM_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_REM_S(x, y) REM_S(u64, INT64_MIN, (s64)x, (s64)y)

#define DIVREM_U(op, x, y) \
  ((UNLIKELY((y) == 0)) ? TRAP(DIV_BY_ZERO) : ((x)op(y)))

#define DIV_U(x, y) DIVREM_U(/, x, y)
#define REM_U(x, y) DIVREM_U(%, x, y)

#define ROTL(x, y, mask) \
  (((x) << ((y) & (mask))) | ((x) >> (((mask) - (y) + 1) & (mask))))
#define ROTR(x, y, mask) \
  (((x) >> ((y) & (mask))) | ((x) << (((mask) - (y) + 1) & (mask))))

#define I32_ROTL(x, y) ROTL(x, y, 31)
#define I64_ROTL(x, y) ROTL(x, y, 63)
#define I32_ROTR(x, y) ROTR(x, y, 31)
#define I64_ROTR(x, y) ROTR(x, y, 63)

#define FMIN(x, y)                                                     \
  ((UNLIKELY((x) != (x)))                                              \
       ? NAN                                                           \
       : (UNLIKELY((y) != (y)))                                        \
             ? NAN                                                     \
             : (UNLIKELY((x) == 0 && (y) == 0)) ? (signbit(x) ? x : y) \
                                                : (x < y) ? x : y)

#define FMAX(x, y)                                                     \
  ((UNLIKELY((x) != (x)))                                              \
       ? NAN                                                           \
       : (UNLIKELY((y) != (y)))                                        \
             ? NAN                                                     \
             : (UNLIKELY((x) == 0 && (y) == 0)) ? (signbit(x) ? y : x) \
                                                : (x > y) ? x : y)

#define TRUNC_S(ut, st, ft, min, minop, max, x)                           \
  ((UNLIKELY((x) != (x)))                                                 \
       ? TRAP(INVALID_CONVERSION)                                         \
       : (UNLIKELY(!((x)minop(min) && (x) < (max)))) ? TRAP(INT_OVERFLOW) \
                                                     : (ut)(st)(x))

#define I32_TRUNC_S_F32(x) \
  TRUNC_S(u32, s32, f32, (f32)INT32_MIN, >=, 2147483648.f, x)
#define I64_TRUNC_S_F32(x) \
  TRUNC_S(u64, s64, f32, (f32)INT64_MIN, >=, (f32)INT64_MAX, x)
#define I32_TRUNC_S_F64(x) \
  TRUNC_S(u32, s32, f64, -2147483649., >, 2147483648., x)
#define I64_TRUNC_S_F64(x) \
  TRUNC_S(u64, s64, f64, (f64)INT64_MIN, >=, (f64)INT64_MAX, x)

#define TRUNC_U(ut, ft, max, x)                                          \
  ((UNLIKELY((x) != (x)))                                                \
       ? TRAP(INVALID_CONVERSION)                                        \
       : (UNLIKELY(!((x) > (ft)-1 && (x) < (max)))) ? TRAP(INT_OVERFLOW) \
                                                    : (ut)(x))

#define I32_TRUNC_U_F32(x) TRUNC_U(u32, f32, 4294967296.f, x)
#define I64_TRUNC_U_F32(x) TRUNC_U(u64, f32, (f32)UINT64_MAX, x)
#define I32_TRUNC_U_F64(x) TRUNC_U(u32, f64, 4294967296., x)
#define I64_TRUNC_U_F64(x) TRUNC_U(u64, f64, (f64)UINT64_MAX, x)

#define TRUNC_SAT_S(ut, st, ft, min, smin, minop, max, smax, x) \
  ((UNLIKELY((x) != (x)))                                       \
       ? 0                                                      \
       : (UNLIKELY(!((x)minop(min))))                           \
             ? smin                                             \
             : (UNLIKELY(!((x) < (max)))) ? smax : (ut)(st)(x))

#define I32_TRUNC_SAT_S_F32(x)                                            \
  TRUNC_SAT_S(u32, s32, f32, (f32)INT32_MIN, INT32_MIN, >=, 2147483648.f, \
              INT32_MAX, x)
#define I64_TRUNC_SAT_S_F32(x)                                              \
  TRUNC_SAT_S(u64, s64, f32, (f32)INT64_MIN, INT64_MIN, >=, (f32)INT64_MAX, \
              INT64_MAX, x)
#define I32_TRUNC_SAT_S_F64(x)                                        \
  TRUNC_SAT_S(u32, s32, f64, -2147483649., INT32_MIN, >, 2147483648., \
              INT32_MAX, x)
#define I64_TRUNC_SAT_S_F64(x)                                              \
  TRUNC_SAT_S(u64, s64, f64, (f64)INT64_MIN, INT64_MIN, >=, (f64)INT64_MAX, \
              INT64_MAX, x)

#define TRUNC_SAT_U(ut, ft, max, smax, x)               \
  ((UNLIKELY((x) != (x))) ? 0                           \
                          : (UNLIKELY(!((x) > (ft)-1))) \
                                ? 0                     \
                                : (UNLIKELY(!((x) < (max)))) ? smax : (ut)(x))

#define I32_TRUNC_SAT_U_F32(x) \
  TRUNC_SAT_U(u32, f32, 4294967296.f, UINT32_MAX, x)
#define I64_TRUNC_SAT_U_F32(x) \
  TRUNC_SAT_U(u64, f32, (f32)UINT64_MAX, UINT64_MAX, x)
#define I32_TRUNC_SAT_U_F64(x) TRUNC_SAT_U(u32, f64, 4294967296., UINT32_MAX, x)
#define I64_TRUNC_SAT_U_F64(x) \
  TRUNC_SAT_U(u64, f64, (f64)UINT64_MAX, UINT64_MAX, x)

#define DEFINE_REINTERPRET(name, t1, t2)         \
  static inline t2 name(t1 x) {                  \
    t2 result;                                   \
    wasm_rt_memcpy(&result, &x, sizeof(result)); \
    return result;                               \
  }

DEFINE_REINTERPRET(f32_reinterpret_i32, u32, f32)
DEFINE_REINTERPRET(i32_reinterpret_f32, f32, u32)
DEFINE_REINTERPRET(f64_reinterpret_i64, u64, f64)
DEFINE_REINTERPRET(i64_reinterpret_f64, f64, u64)

static float quiet_nanf(float x) {
  uint32_t tmp;
  wasm_rt_memcpy(&tmp, &x, 4);
  tmp |= 0x7fc00000lu;
  wasm_rt_memcpy(&x, &tmp, 4);
  return x;
}

static double quiet_nan(double x) {
  uint64_t tmp;
  wasm_rt_memcpy(&tmp, &x, 8);
  tmp |= 0x7ff8000000000000llu;
  wasm_rt_memcpy(&x, &tmp, 8);
  return x;
}

static double wasm_quiet(double x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nan(x);
  }
  return x;
}

static float wasm_quietf(float x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nanf(x);
  }
  return x;
}

static double wasm_floor(double x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nan(x);
  }
  return floor(x);
}

static float wasm_floorf(float x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nanf(x);
  }
#if __STDC_VERSION__ >= 199901L
  return floorf(x);
#else
  return floor(x);
#endif
}

static double wasm_ceil(double x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nan(x);
  }
  return ceil(x);
}

static float wasm_ceilf(float x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nanf(x);
  }
#if __STDC_VERSION__ >= 199901L
  return ceilf(x);
#else
  return ceil(x);
#endif
}

static double wasm_trunc(double x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nan(x);
  }
  return trunc(x);
}

static float wasm_truncf(float x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nanf(x);
  }
#if __STDC_VERSION__ >= 199901L
  return truncf(x);
#else
  return trunc(x);
#endif
}

#if __STDC_VERSION__ >= 199901L
static float wasm_nearbyintf(float x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nanf(x);
  }
  return nearbyintf(x);
}

static double wasm_nearbyint(double x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nan(x);
  }
  return nearbyint(x);
}
#endif

static float wasm_fabsf(float x) {
  if (UNLIKELY(isnan(x))) {
    uint32_t tmp;
    wasm_rt_memcpy(&tmp, &x, 4);
    tmp = tmp & ~(1UL << 31);
    wasm_rt_memcpy(&x, &tmp, 4);
    return x;
  }
#if __STDC_VERSION__ >= 199901L
  return fabsf(x);
#else
  return fabs(x);
#endif
}

static double wasm_fabs(double x) {
  if (UNLIKELY(isnan(x))) {
    uint64_t tmp;
    wasm_rt_memcpy(&tmp, &x, 8);
    tmp = tmp & ~(1ULL << 63);
    wasm_rt_memcpy(&x, &tmp, 8);
    return x;
  }
  return fabs(x);
}

static double wasm_sqrt(double x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nan(x);
  }
  return sqrt(x);
}

static float wasm_sqrtf(float x) {
  if (UNLIKELY(isnan(x))) {
    return quiet_nanf(x);
  }
#if __STDC_VERSION__ >= 199901L
  return sqrtf(x);
#else
  return sqrt(x);
#endif
}

static inline void memory_fill(wasm_rt_memory_t* mem, u32 d, u32 val, u32 n) {
  RANGE_CHECK(mem, d, n);
  memset(MEM_ADDR(mem, d, n), val, n);
}

static inline void memory_copy(wasm_rt_memory_t* dest,
                               const wasm_rt_memory_t* src,
                               u32 dest_addr,
                               u32 src_addr,
                               u32 n) {
  RANGE_CHECK(dest, dest_addr, n);
  RANGE_CHECK(src, src_addr, n);
  memmove(MEM_ADDR(dest, dest_addr, n), MEM_ADDR(src, src_addr, n), n);
}

static inline void memory_init(wasm_rt_memory_t* dest,
                               const u8* src,
                               u32 src_size,
                               u32 dest_addr,
                               u32 src_addr,
                               u32 n) {
  if (UNLIKELY(src_addr + (uint64_t)n > src_size))
    TRAP(OOB);
  LOAD_DATA((*dest), dest_addr, src + src_addr, n);
}

typedef struct {
  enum { RefFunc, RefNull, GlobalGet } expr_type;
  wasm_rt_func_type_t type;
  wasm_rt_function_ptr_t func;
  wasm_rt_tailcallee_t func_tailcallee;
  size_t module_offset;
} wasm_elem_segment_expr_t;

static inline void funcref_table_init(wasm_rt_funcref_table_t* dest,
                                      const wasm_elem_segment_expr_t* src,
                                      u32 src_size,
                                      u32 dest_addr,
                                      u32 src_addr,
                                      u32 n,
                                      void* module_instance) {
  u32 i;
  const wasm_elem_segment_expr_t* src_expr;
  wasm_rt_funcref_t* dest_val;
  if (UNLIKELY(src_addr + (uint64_t)n > src_size))
    TRAP(OOB);
  if (UNLIKELY(dest_addr + (uint64_t)n > dest->size))
    TRAP(OOB);
  for (i = 0; i < n; i++) {
    src_expr = &src[src_addr + i];
    dest_val = &(dest->data[dest_addr + i]);
    switch (src_expr->expr_type) {
      case RefFunc:
        dest_val->func_type = src_expr->type;
        dest_val->func = src_expr->func;
        dest_val->func_tailcallee = src_expr->func_tailcallee;
        dest_val->module_instance = (char *) module_instance +
            src_expr->module_offset;
        break;
      case RefNull:
        wasm_rt_funcref_nullify(dest_val);
        break;
      case GlobalGet:
        *dest_val = **(wasm_rt_funcref_t**)((char*)module_instance +
                                            src_expr->module_offset);
        break;
    }
  }
}

// Currently wasm2c only supports initializing externref tables with ref.null.
static inline void externref_table_init(wasm_rt_externref_table_t* dest,
                                        u32 src_size,
                                        u32 dest_addr,
                                        u32 src_addr,
                                        u32 n) {
  u32 i;
  if (UNLIKELY(src_addr + (uint64_t)n > src_size))
    TRAP(OOB);
  if (UNLIKELY(dest_addr + (uint64_t)n > dest->size))
    TRAP(OOB);
  memset((unsigned char *) dest->data + dest_addr, 0, n);
}

#define DEFINE_TABLE_COPY(type)                                              \
  static inline void type##_table_copy(wasm_rt_##type##_table_t* dest,       \
                                       const wasm_rt_##type##_table_t* src,  \
                                       u32 dest_addr, u32 src_addr, u32 n) { \
    if (UNLIKELY(dest_addr + (uint64_t)n > dest->size))                      \
      TRAP(OOB);                                                             \
    if (UNLIKELY(src_addr + (uint64_t)n > src->size))                        \
      TRAP(OOB);                                                             \
                                                                             \
    memmove(dest->data + dest_addr, src->data + src_addr,                    \
            n * sizeof(wasm_rt_##type##_t));                                 \
  }

DEFINE_TABLE_COPY(funcref)
DEFINE_TABLE_COPY(externref)

#define DEFINE_TABLE_GET(type)                        \
  static inline wasm_rt_##type##_t type##_table_get(  \
      const wasm_rt_##type##_table_t* table, u32 i) { \
    if (UNLIKELY(i >= table->size))                   \
      TRAP(OOB);                                      \
    return table->data[i];                            \
  }

DEFINE_TABLE_GET(funcref)
DEFINE_TABLE_GET(externref)

#define DEFINE_TABLE_SET(type)                                               \
  static inline void type##_table_set(const wasm_rt_##type##_table_t* table, \
                                      u32 i, const wasm_rt_##type##_t val) { \
    if (UNLIKELY(i >= table->size))                                          \
      TRAP(OOB);                                                             \
    table->data[i] = val;                                                    \
  }

DEFINE_TABLE_SET(funcref)
DEFINE_TABLE_SET(externref)

#define DEFINE_TABLE_FILL(type)                                               \
  static inline void type##_table_fill(const wasm_rt_##type##_table_t* table, \
                                       u32 d, const wasm_rt_##type##_t val,   \
                                       u32 n) {                               \
    uint32_t i;                                                               \
    if (UNLIKELY((uint64_t)d + n > table->size))                              \
      TRAP(OOB);                                                              \
    for (i = d; i < d + n; i++) {                                             \
      table->data[i] = val;                                                   \
    }                                                                         \
  }

DEFINE_TABLE_FILL(funcref)
DEFINE_TABLE_FILL(externref)

#if defined(__GNUC__) || defined(__clang__)
#define FUNC_TYPE_DECL_EXTERN_T(x) extern const char* const x
#define FUNC_TYPE_EXTERN_T(x) const char* const x
#define FUNC_TYPE_T(x) static const char* const x
#else
#define FUNC_TYPE_DECL_EXTERN_T(x) extern const char x[]
#define FUNC_TYPE_EXTERN_T(x) const char x[]
#define FUNC_TYPE_T(x) static const char x[]
#endif

#if (__STDC_VERSION__ < 201112L) && !defined(static_assert)
#define static_assert(X) \
  extern int(*assertion(void))[!!sizeof(struct { int x : (X) ? 2 : -1; })];
#endif

#ifdef _MSC_VER
#define WEAK_FUNC_DECL(func, fallback)                             \
  __pragma(comment(linker, "/alternatename:" #func "=" #fallback)) \
                                                                   \
      void                                                         \
      fallback(void** instance_ptr, void* tail_call_stack,         \
               wasm_rt_tailcallee_t* next)
#else
#define WEAK_FUNC_DECL(func, fallback)                                        \
  __attribute__((weak)) void func(void** instance_ptr, void* tail_call_stack, \
                                  wasm_rt_tailcallee_t* next)
#endif
