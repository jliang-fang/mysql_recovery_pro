#ifndef CPUID_H
#define CPUID_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* CPUID leaf functions */
#define CPUID_GETVENDORSTRING      0x00000000
#define CPUID_GETFEATURES          0x00000001
#define CPUID_GETCACHEPARAMS       0x00000002
#define CPUID_GETTLB               0x00000003
#define CPUID_GETSERIAL            0x00000003
#define CPUID_GETBRANDSTRING       0x80000002
#define CPUID_GETBRANDSTRINGEND    0x80000004

/* EAX=1 feature flags (in EDX and ECX) */
#define CPUID_FEATURE_SSE3     (1 << 0)
#define CPUID_FEATURE_PCLMUL   (1 << 1)
#define CPUID_FEATURE_DTES64   (1 << 2)
#define CPUID_FEATURE_MONITOR  (1 << 3)
#define CPUID_FEATURE_DS_CPL   (1 << 4)
#define CPUID_FEATURE_VMX      (1 << 5)
#define CPUID_FEATURE_SMX      (1 << 6)
#define CPUID_FEATURE_EST      (1 << 7)
#define CPUID_FEATURE_TM2      (1 << 8)
#define CPUID_FEATURE_SSSE3    (1 << 9)
#define CPUID_FEATURE_CNXT_ID  (1 << 10)
#define CPUID_FEATURE_SDBG     (1 << 11)
#define CPUID_FEATURE_FMA      (1 << 12)
#define CPUID_FEATURE_CX16     (1 << 13)
#define CPUID_FEATURE_XTPR     (1 << 14)
#define CPUID_FEATURE_PDCM     (1 << 15)
#define CPUID_FEATURE_PCID     (1 << 17)
#define CPUID_FEATURE_DCA      (1 << 18)
#define CPUID_FEATURE_SSE4_1   (1 << 19)
#define CPUID_FEATURE_SSE4_2   (1 << 20)
#define CPUID_FEATURE_X2APIC   (1 << 21)
#define CPUID_FEATURE_MOVBE    (1 << 22)
#define CPUID_FEATURE_POPCNT   (1 << 23)
#define CPUID_FEATURE_TSC_DEADLINE (1 << 24)
#define CPUID_FEATURE_AES      (1 << 25)
#define CPUID_FEATURE_XSAVE    (1 << 26)
#define CPUID_FEATURE_OSXSAVE  (1 << 27)
#define CPUID_FEATURE_AVX      (1 << 28)
#define CPUID_FEATURE_F16C     (1 << 29)
#define CPUID_FEATURE_RDRAND   (1 << 30)

/* Extended features (EAX=0x80000001, EDX) */
#define CPUID_XFEATURE_LAHF    (1 << 0)
#define CPUID_XFEATURE_LM      (1 << 29)
#define CPUID_XFEATURE_SYSCALL (1 << 11)

/* Vendor strings */
#define CPUID_VENDOR_INTEL "GenuineIntel"
#define CPUID_VENDOR_AMD   "AuthenticAMD"
#define CPUID_VENDOR_OTHER "UnknownCPU"

/* Structure to hold CPUID result */
typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} cpuid_result_t;

/* Main function: execute CPUID instruction with given EAX (and ECX if needed) */
cpuid_result_t cpuid(uint32_t leaf, uint32_t subleaf);

/* Helper functions */
const char* cpuid_get_vendor_string(void);
int cpuid_has_feature(uint32_t feature_flag);
void cpuid_get_brand_string(char *buffer);

#ifdef __cplusplus
}
#endif

#endif /* CPUID_H */
