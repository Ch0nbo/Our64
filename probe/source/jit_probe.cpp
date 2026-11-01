#include "jit_probe.h"
#include "report.h"

#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <coreinit/thread.h>

#include <stdint.h>
#include <stdlib.h>

// The tests write PowerPC code by hand, so the encodings are spelled out here.
namespace ppc {
constexpr uint32_t LI_R3_42 = 0x3860002A;   // li r3, 42
constexpr uint32_t BLR = 0x4E800020;        // blr
constexpr uint32_t STWU_R1 = 0x9421FFF0;    // stwu r1, -16(r1)
constexpr uint32_t MFLR_R0 = 0x7C0802A6;    // mflr r0
constexpr uint32_t STW_R0 = 0x90010014;     // stw r0, 20(r1)
constexpr uint32_t LWZ_R0 = 0x80010014;     // lwz r0, 20(r1)
constexpr uint32_t MTLR_R0 = 0x7C0803A6;    // mtlr r0
constexpr uint32_t ADDI_R1 = 0x38210010;    // addi r1, r1, 16
constexpr uint32_t MTCTR_R12 = 0x7D8903A6;  // mtctr r12
constexpr uint32_t BCTRL = 0x4E800421;      // bctrl
inline uint32_t LisR12(uint32_t v) { return 0x3D800000 | (v >> 16); }
inline uint32_t OriR12(uint32_t v) { return 0x618C0000 | (v & 0xFFFF); }
inline uint32_t Bl(const void *from, const void *to)
{
   int32_t disp = (int32_t)((uintptr_t)to - (uintptr_t)from);
   return 0x48000001 | ((uint32_t)disp & 0x03FFFFFC);
}
constexpr int32_t BRANCH_REACH = 32 * 1024 * 1024;   // +-32 MB for b/bl
}

extern "C" __attribute__((noinline)) int Our64_ProbeHelper(int x)
{
   return x + 1;
}

namespace {

void *g_base = nullptr;
uint32_t g_size = 0;

// Writes `n` words at `dst` inside one writable window and makes them runnable.
bool WriteCode(uint32_t *dst, const uint32_t *src, int n)
{
   if (!OSSwitchSecCodeGenMode(CODEGEN_RW_))
      return false;
   for (int i = 0; i < n; i++)
      dst[i] = src[i];
   DCFlushRange(dst, n * 4);
   OSSwitchSecCodeGenMode(CODEGEN_R_X);
   ICInvalidateRange(dst, n * 4);
   return true;
}

bool InReach(const void *from, const void *to)
{
   int64_t d = (int64_t)(uintptr_t)to - (int64_t)(uintptr_t)from;
   return d > -ppc::BRANCH_REACH && d < ppc::BRANCH_REACH;
}

int32_t ProbeThread(int argc, const char **argv)
{
   uint32_t *code = (uint32_t *)g_base;
   Report_Line("[a] running on core %u", OSGetCoreId());

   if (!OSSwitchSecCodeGenMode(CODEGEN_RW_)) {
      Report_Line("[a] FAIL: cannot make the JIT area writable from this core");
      return 1;
   }
   OSSwitchSecCodeGenMode(CODEGEN_R_X);

   // 1. Smallest possible generated function.
   const uint32_t ret42[] = {ppc::LI_R3_42, ppc::BLR};
   WriteCode(code, ret42, 2);
   int r = ((int (*)())code)();
   Report_Line("[a] write + run generated code: %s (returned %d, want 42)", r == 42 ? "OK" : "FAIL", r);

   // 2. Generated code calling a C helper with a relative bl, as Not64's recompiler does.
   uint32_t *fn = code + 8;
   bool reach = InReach(fn + 3, (void *)&Our64_ProbeHelper);
   if (reach) {
      uint32_t rel[] = {ppc::STWU_R1, ppc::MFLR_R0, ppc::STW_R0, 0,
                        ppc::LWZ_R0, ppc::MTLR_R0, ppc::ADDI_R1, ppc::BLR};
      rel[3] = ppc::Bl(fn + 3, (void *)&Our64_ProbeHelper);
      WriteCode(fn, rel, 8);
      r = ((int (*)(int))fn)(41);
      Report_Line("[a] relative bl to C helper: %s (returned %d, want 42)", r == 42 ? "OK" : "FAIL", r);
   } else {
      Report_Line("[a] relative bl to C helper: OUT OF REACH from %p", fn);
   }

   // 3. The fallback: absolute call through the count register.
   uint32_t *fn2 = code + 32;
   uint32_t helper = (uint32_t)(uintptr_t)&Our64_ProbeHelper;
   const uint32_t absCall[] = {ppc::STWU_R1, ppc::MFLR_R0, ppc::STW_R0,
                               ppc::LisR12(helper), ppc::OriR12(helper), ppc::MTCTR_R12, ppc::BCTRL,
                               ppc::LWZ_R0, ppc::MTLR_R0, ppc::ADDI_R1, ppc::BLR};
   WriteCode(fn2, absCall, 11);
   r = ((int (*)(int))fn2)(41);
   Report_Line("[a] absolute call to C helper: %s (returned %d, want 42)", r == 42 ? "OK" : "FAIL", r);

   // 4. The last bytes of the area are usable, so the reported size is real.
   uint32_t *tail = (uint32_t *)((uint8_t *)g_base + g_size - 8);
   WriteCode(tail, ret42, 2);
   r = ((int (*)())tail)();
   Report_Line("[a] last 8 bytes of area run: %s", r == 42 ? "OK" : "FAIL");

   // b1. Bare mode switch cost.
   const int N = 10000;
   double t0 = Report_NowUs();
   for (int i = 0; i < N; i++) {
      OSSwitchSecCodeGenMode(CODEGEN_RW_);
      OSSwitchSecCodeGenMode(CODEGEN_R_X);
   }
   double perSwitch = (Report_NowUs() - t0) / N;
   Report_Line("[b] writable->executable round trip: %.2f us", perSwitch);

   // b2. What one block link costs: Not64 rewrites 3 words (lis, ori, b) per link.
   uint32_t *patch = code + 64;
   const uint32_t link[] = {ppc::LisR12(0x80001234), ppc::OriR12(0x80001234), ppc::BLR};
   t0 = Report_NowUs();
   for (int i = 0; i < N; i++)
      WriteCode(patch, link, 3);
   double perPatch = (Report_NowUs() - t0) / N;
   Report_Line("[b] one 3-word link patch (switch, write, flush, switch, invalidate): %.2f us", perPatch);

   // b3. Same patch through OSCodegenCopy.
   t0 = Report_NowUs();
   for (int i = 0; i < N; i++) {
      OSCodegenCopy(patch, (void *)link, sizeof(link));
      ICInvalidateRange(patch, sizeof(link));
   }
   Report_Line("[b] same patch via OSCodegenCopy: %.2f us", (Report_NowUs() - t0) / N);

   // b4. Bulk write speed: what compiling a large batch of blocks costs.
   uint32_t bulk = g_size < 1024 * 1024 ? g_size / 2 : 1024 * 1024;
   uint32_t *dst = (uint32_t *)((uint8_t *)g_base + 4096);
   t0 = Report_NowUs();
   OSSwitchSecCodeGenMode(CODEGEN_RW_);
   for (uint32_t i = 0; i < bulk / 4; i++)
      dst[i] = ppc::BLR;
   DCFlushRange(dst, bulk);
   OSSwitchSecCodeGenMode(CODEGEN_R_X);
   ICInvalidateRange(dst, bulk);
   double bulkUs = Report_NowUs() - t0;
   Report_Line("[b] bulk write %u KB in one window: %.0f us (%.1f MB/s)", bulk / 1024, bulkUs,
               (bulk / 1048576.0) / (bulkUs / 1e6));

   const char *verdict = perSwitch < 10.0 ? "OK: patch blocks one at a time"
                                          : "RESHAPE: batch block links per window";
   Report_Line("[b] verdict: %s (threshold 10 us per round trip)", verdict);
   return 0;
}

alignas(16) OSThread g_thread;
alignas(16) uint8_t g_stack[128 * 1024];

bool RunOnCore(uint32_t core)
{
   OSThreadAttributes attr = (OSThreadAttributes)(1u << core);
   if (!OSCreateThread(&g_thread, ProbeThread, 0, nullptr, g_stack + sizeof(g_stack), sizeof(g_stack), 16,
                       attr)) {
      Report_Line("[a] could not create a thread on core %u", core);
      return false;
   }
   OSResumeThread(&g_thread);
   int ret = 1;
   OSJoinThread(&g_thread, &ret);
   return ret == 0;
}

}

void JitProbe_Run()
{
   OSGetCodegenVirtAddrRange(&g_base, &g_size);
   uint32_t core = OSGetCodegenCore();
   Report_Line("[a] JIT area: base=%p size=%u KB (%.1f MB), codegen core=0x%08X, mode=0x%X", g_base,
               (unsigned)(g_size / 1024), g_size / 1048576.0, (unsigned)core, (unsigned)OSGetCodegenMode());

   if (!g_base || !g_size) {
      Report_Line("[a] verdict: STOP: no JIT area granted to this launch");
      return;
   }

   const void *helper = (void *)&Our64_ProbeHelper;
   int64_t dLo = (int64_t)(uintptr_t)g_base - (int64_t)(uintptr_t)helper;
   int64_t dHi = (int64_t)((uintptr_t)g_base + g_size) - (int64_t)(uintptr_t)helper;
   int64_t far = llabs(dLo) > llabs(dHi) ? llabs(dLo) : llabs(dHi);
   Report_Line("[a] app code at %p; farthest JIT byte is %.1f MB away (relative branch reach 32 MB)", helper,
               far / 1048576.0);

   bool ok;
   if (core <= 2) {
      ok = RunOnCore(core);
   } else {
      // Unknown encoding: find the core that is allowed to switch modes.
      ok = false;
      for (uint32_t c = 0; c < 3 && !ok; c++) {
         Report_Line("[a] codegen core value unclear, trying core %u", (unsigned)c);
         ok = RunOnCore(c);
      }
   }

   const char *size = g_size >= 8u * 1024 * 1024 ? "OK" : "STOP (under 8 MB)";
   const char *reach = far < ppc::BRANCH_REACH ? "OK" : "RESHAPE (helper calls need absolute calls)";
   Report_Line("[a] verdict: size %s; reach %s; tests %s", size, reach, ok ? "ran" : "FAILED");
}
