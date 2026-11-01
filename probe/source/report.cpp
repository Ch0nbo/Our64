#include "report.h"

#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/sdcard.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {
char g_buf[64 * 1024];
size_t g_len = 0;
char g_path[256] = "";
bool g_mounted = false;
}

void Report_Line(const char *fmt, ...)
{
   char line[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);

   WHBLogPrint(line);

   size_t n = strlen(line);
   if (g_len + n + 2 < sizeof(g_buf)) {
      memcpy(g_buf + g_len, line, n);
      g_len += n;
      g_buf[g_len++] = '\n';
      g_buf[g_len] = '\0';
   }
}

bool Report_Save()
{
   if (!g_mounted) {
      if (!WHBMountSdCard()) {
         WHBLogPrint("SD card mount failed: results only on screen and UDP");
         return false;
      }
      g_mounted = true;
      char dir[200];
      snprintf(dir, sizeof(dir), "%s/wiiu", WHBGetSdCardMountPath());
      mkdir(dir, 0777);
      snprintf(dir, sizeof(dir), "%s/wiiu/our64", WHBGetSdCardMountPath());
      mkdir(dir, 0777);
      snprintf(g_path, sizeof(g_path), "%s/probe-results.txt", dir);
   }

   FILE *f = fopen(g_path, "w");
   if (!f)
      return false;
   fwrite(g_buf, 1, g_len, f);
   fclose(f);
   return true;
}

void Report_Replay()
{
   const char *p = g_buf;
   while (*p) {
      const char *nl = strchr(p, '\n');
      size_t n = nl ? (size_t)(nl - p) : strlen(p);
      char line[512];
      if (n >= sizeof(line))
         n = sizeof(line) - 1;
      memcpy(line, p, n);
      line[n] = '\0';
      WHBLogPrint(line);
      p += n + (nl ? 1 : 0);
   }
}

const char *Report_Path()
{
   return g_path[0] ? g_path : "(not saved)";
}

double Report_NowUs()
{
   return (double)OSTicksToMicroseconds(OSGetSystemTime());
}
