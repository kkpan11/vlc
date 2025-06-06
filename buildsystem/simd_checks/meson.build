# SIMD checks

enable_sse = false
enable_avx = false
if host_machine.cpu_family().startswith('x86')
    enable_sse = get_option('sse').allowed()
    enable_avx = get_option('avx').allowed()
endif

# Check for fully workin SSE2 intrinsics
have_sse2_intrinsics = enable_sse and cc.compiles('''
    #include <emmintrin.h>
    #include <stdint.h>
    uint64_t frobzor;

    void f() {
        __m128i a, b, c;
        a = b = c = _mm_set1_epi64((__m64)frobzor);
        a = _mm_slli_epi16(a, 3);
        a = _mm_adds_epi16(a, b);
        c = _mm_srli_epi16(c, 8);
        c = _mm_slli_epi16(c, 3);
        b = _mm_adds_epi16(b, c);
        a = _mm_unpacklo_epi8(a, b);
        frobzor = (uint64_t)_mm_movepi64_pi64(a);
    }
''', args: ['-msse2'], name: 'SSE2 intrinsics check')
if have_sse2_intrinsics
    cdata.set('HAVE_SSE2_INTRINSICS', 1)
endif

# Check for SSE2 inline assembly support
can_compile_sse2 = enable_sse and cc.compiles('''
    void f() {
        void *p;
        asm volatile("punpckhqdq %%xmm1,%%xmm2"::"r"(p):"xmm1", "xmm2");
    }
''', args: ['-msse'], name: 'SSE2 inline asm check')
if can_compile_sse2
    cdata.set('CAN_COMPILE_SSE2', 1)
endif
have_sse2 = can_compile_sse2

# Check for SSE3 inline assembly support
can_compile_sse3 = enable_sse and cc.compiles('''
    void f() {
        void *p;
        asm volatile("movsldup %%xmm1,%%xmm0"::"r"(p):"xmm0", "xmm1");
    }
''', args: ['-msse'], name: 'SSE3 inline asm check')
if can_compile_sse3
    cdata.set('CAN_COMPILE_SSE3', 1)
endif

# Check for SSSE3 inline assembly support
can_compile_2_sse3 = enable_sse and cc.compiles('''
    void f() {
        void *p;
        asm volatile("pabsw %%xmm0,%%xmm0"::"r"(p):"xmm0");
    }
''', args: ['-msse'], name: 'SSSE3 inline asm check')
if can_compile_2_sse3
    cdata.set('CAN_COMPILE_SSSE3', 1)
endif
have_sse3 = can_compile_sse3 and can_compile_2_sse3

# Check for SSE4.1 inline assembly support
can_compile_sse4_1 = enable_sse and cc.compiles('''
    void f() {
        void *p;
        asm volatile("pmaxsb %%xmm1,%%xmm0"::"r"(p):"xmm0", "xmm1");
    }
''', args: ['-msse'], name: 'SSE4.1 inline asm check')
if can_compile_sse4_1
    cdata.set('CAN_COMPILE_SSE4_1', 1)
endif
have_sse4_1 = can_compile_sse4_1

# Check for SSE4.2 inline assembly support
can_compile_sse4_2 = enable_sse and cc.compiles('''
    void f() {
        void *p;
        asm volatile("pcmpgtq %%xmm1,%%xmm0"::"r"(p):"xmm0", "xmm1");
    }
''', args: ['-msse'], name: 'SSE4.2 inline asm check')
if can_compile_sse4_2
    cdata.set('CAN_COMPILE_SSE4_2', 1)
endif
have_sse4_2 = can_compile_sse4_2

# Check for SSE4A inline assembly support
can_compile_sse4A = enable_sse and cc.compiles('''
    void f() {
        void *p;
        asm volatile("insertq %%xmm1,%%xmm0"::"r"(p):"xmm0", "xmm1");
    }
''', args: ['-msse'], name: 'SSE4A inline asm check')
if can_compile_sse4A
    cdata.set('CAN_COMPILE_SSE4A', 1)
endif
have_sse4A = can_compile_sse4A

# Check for fully workin AVX2 intrinsics
have_avx2_intrinsics = enable_avx and cc.compiles('''
    #include <immintrin.h>
    #include <stdint.h>
    uint64_t frobzor;

    void f() {
        __m256i a, b, c;
        a = b = c = _mm256_set1_epi64x((int64_t)frobzor);
        a = _mm256_slli_epi16(a, 3);
        a = _mm256_adds_epi16(a, b);
        c = _mm256_srli_epi16(c, 8);
        c = _mm256_slli_epi16(c, 3);
        b = _mm256_adds_epi16(b, c);
        a = _mm256_unpacklo_epi8(a, b);
        frobzor = (uint64_t)_mm256_extract_epi64(a, 0);
    }
''', args: ['-mavx2'], name: 'AVX2 intrinsics check')
if have_avx2_intrinsics
    cdata.set('HAVE_AVX2_INTRINSICS', 1)
endif

# Check for AVX inline assembly support
can_compile_avx = enable_avx and cc.compiles('''
    void f() {
        void *p;
        asm volatile("vxorps %%ymm1,%%ymm2,%%ymm3"::"r"(p):"ymm1", "ymm2", "ymm3");
    }
''', args: ['-mavx'], name: 'AVX inline asm check')
if can_compile_avx
    cdata.set('CAN_COMPILE_AVX', 1)
endif
have_avx = can_compile_avx

# Check for AVX2 inline assembly support
can_compile_avx2 = enable_avx and cc.compiles('''
    void f() {
        void *p;
        asm volatile("vpunpckhqdq %%ymm1,%%ymm2,%%ymm3"::"r"(p):"ymm1", "ymm2", "ymm3");
    }
''', args: ['-mavx'], name: 'AVX2 inline asm check')
if can_compile_avx2
    cdata.set('CAN_COMPILE_AVX2', 1)
endif
have_avx2 = can_compile_avx2

# TODO: ARM Neon checks and SVE checks
# TODO: Altivec checks
