// Our64 stage-0 hardware probe.
// Measures what the feasibility study left open: the JIT area Aroma grants,
// the cost of switching it, uber-shader draw cost, runtime shader compile time
// and frame readback cost. Results: screen, OSReport, UDP (port 4405) and
// sd:/wiiu/our64/probe-results.txt.

#include "gpu_probe.h"
#include "jit_probe.h"
#include "report.h"

#include <coreinit/memdefaultheap.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <whb/log_cafe.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

// Largest single block the default heap hands out, found by trying sizes from 1 GB down.
static unsigned LargestAllocMB()
{
   for (unsigned mb = 1024; mb >= 16; mb -= 16) {
      void *p = MEMAllocFromDefaultHeapEx(mb << 20, 64);
      if (p) {
         MEMFreeToDefaultHeap(p);
         return mb;
      }
   }
   return 0;
}

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogCafeInit();
   WHBLogUdpInit();
   WHBLogConsoleInit();

   Report_Line("Our64 stage-0 probe (" __DATE__ " " __TIME__ ")");
   Report_Line("title id %016llX, bus clock %u Hz", OSGetTitleID(), (unsigned)OSGetSystemInfo()->busClockSpeed);
   Report_Line("largest single allocation: %u MB (16 MB steps)", LargestAllocMB());
   WHBLogConsoleDraw();

   JitProbe_Run();
   Report_Save();
   Report_Line("JIT tests done. GPU tests next: the screen turns red, yellow, green, then black.");
   WHBLogConsoleDraw();
   OSSleepTicks(OSMillisecondsToTicks(2000));
   WHBLogConsoleFree();

   GpuProbe_Run();

   WHBLogConsoleInit();
   Report_Replay();
   bool saved = Report_Save();
   Report_Line(saved ? "Saved to %s" : "SD save failed (%s); results were also sent over UDP", Report_Path());
   Report_Save();
   Report_Line("Done. Press HOME to exit.");

   while (WHBProcIsRunning()) {
      WHBLogConsoleDraw();
      OSSleepTicks(OSMillisecondsToTicks(100));
   }

   WHBLogConsoleFree();
   WHBLogUdpDeinit();
   WHBLogCafeDeinit();
   WHBProcShutdown();
   return 0;
}
