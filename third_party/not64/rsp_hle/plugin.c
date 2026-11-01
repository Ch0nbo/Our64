/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - plugin.c                                        *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <stdarg.h>
#include <stdio.h>

#include "wintypes.h"
#include "common.h"
#include "hle.h"
#include "hle_internal.h"
#include "hle_external.h"

#include "RSPPlugin.h"
#include "Rsp_#1.1.h"
#include "Audio_#1.1.h"

/* local variables */
static struct hle_t g_hle;
static void (*l_CheckInterrupts)(void) = NULL;
static void (*l_ProcessDlistList)(void) = NULL;
static void (*l_ProcessAlistList)(void) = NULL;
static void (*l_ProcessRdpList)(void) = NULL;
static void (*l_ShowCFB)(void) = NULL;

/* Our64: audio lists on core 2 (plugins.c). A second HLE state with its own
 * cached ucodes and audio state; the RSP and MI registers it writes are
 * plugins.c's copies, applied on the CPU thread, so it checks no interrupts. */
static struct hle_t g_hle_audio;
static int g_hle_audio_bound;

/* Global functions needed by HLE core */
void HleVerboseMessage(void* UNUSED(user_defined), const char *message, ...)
{
    /* discard verbose message */
}

void HleInfoMessage(void* UNUSED(user_defined), const char *message, ...)
{
    /* discard informational message */
}

void HleErrorMessage(void* UNUSED(user_defined), const char *message, ...)
{
    /* discard error message */
}

void HleWarnMessage(void* UNUSED(user_defined), const char *message, ...)
{
    /* discard warning message */
}

void HleCheckInterrupts(void* user_defined)
{
    if (l_CheckInterrupts == NULL || user_defined == &g_hle_audio)
        return;

    (*l_CheckInterrupts)();
}

void HleProcessDlistList(void* UNUSED(user_defined))
{
    if (l_ProcessDlistList == NULL)
        return;

    (*l_ProcessDlistList)();
}

void HleProcessAlistList(void* UNUSED(user_defined))
{
    if (l_ProcessAlistList == NULL)
        return;

    (*l_ProcessAlistList)();
}

void HleProcessRdpList(void* UNUSED(user_defined))
{
    if (l_ProcessRdpList == NULL)
        return;

    (*l_ProcessRdpList)();
}

void HleShowCFB(void* UNUSED(user_defined))
{
    if (l_ShowCFB == NULL)
        return;

    (*l_ShowCFB)();
}

int HleForwardTask(void* user_defined)
{
    return -1;
}

/* DLL-exported functions */
EXPORT void CALL CloseDLL(void)
{
}

EXPORT unsigned int CALL DoRspCycles(unsigned int Cycles)
{
    hle_execute(&g_hle);
    return Cycles;
}

EXPORT void CALL InitiateRSP(RSP_INFO Rsp_Info, unsigned int* UNUSED(CycleCount))
{
    hle_init(&g_hle,
             Rsp_Info.RDRAM,
             Rsp_Info.DMEM,
             Rsp_Info.IMEM,
             Rsp_Info.MI_INTR_REG,
             Rsp_Info.SP_MEM_ADDR_REG,
             Rsp_Info.SP_DRAM_ADDR_REG,
             Rsp_Info.SP_RD_LEN_REG,
             Rsp_Info.SP_WR_LEN_REG,
             Rsp_Info.SP_STATUS_REG,
             Rsp_Info.SP_DMA_FULL_REG,
             Rsp_Info.SP_DMA_BUSY_REG,
             Rsp_Info.SP_PC_REG,
             Rsp_Info.SP_SEMAPHORE_REG,
             Rsp_Info.DPC_START_REG,
             Rsp_Info.DPC_END_REG,
             Rsp_Info.DPC_CURRENT_REG,
             Rsp_Info.DPC_STATUS_REG,
             Rsp_Info.DPC_CLOCK_REG,
             Rsp_Info.DPC_BUFBUSY_REG,
             Rsp_Info.DPC_PIPEBUSY_REG,
             Rsp_Info.DPC_TMEM_REG,
             NULL);

    l_CheckInterrupts = Rsp_Info.CheckInterrupts;
    l_ProcessDlistList = Rsp_Info.ProcessDlistList;
    l_ProcessAlistList = Rsp_Info.ProcessAlistList;
    l_ProcessRdpList = Rsp_Info.ProcessRdpList;
    l_ShowCFB = Rsp_Info.ShowCFB;

    g_hle.hle_gfx = 1;
    g_hle.hle_aud = 0;
}

EXPORT void CALL RomClosed(void)
{
    g_hle.cached_ucodes.count = 0;
    g_hle_audio.cached_ucodes.count = 0;
}

/* Once, after InitiateRSP: the copies the audio state writes. sp holds
 * SP_MEM_ADDR, DRAM_ADDR, RD_LEN, WR_LEN, STATUS, DMA_FULL, DMA_BUSY and
 * SEMAPHORE; dpc START, END, CURRENT, STATUS, CLOCK, BUFBUSY, PIPEBUSY, TMEM. */
void our64_hle_audio_bind(unsigned int* mi_intr, unsigned int* const sp[8], unsigned int* sp_pc,
                          unsigned int* const dpc[8])
{
    hle_init(&g_hle_audio, g_hle.dram, g_hle.dmem, g_hle.imem, mi_intr,
             sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp_pc, sp[7],
             dpc[0], dpc[1], dpc[2], dpc[3], dpc[4], dpc[5], dpc[6], dpc[7], &g_hle_audio);
    g_hle_audio.hle_gfx = g_hle.hle_gfx;
    g_hle_audio.hle_aud = g_hle.hle_aud;
    g_hle_audio.cached_ucodes.count = 0;
    g_hle_audio_bound = 1;
}

/* On the CPU thread: the task in DMEM is an audio list core 2 can run. */
int our64_hle_audio_task(void)
{
    return g_hle_audio_bound && hle_audio_task(&g_hle);
}

/* On core 2: runs it against the copies. */
void our64_hle_audio_run(void)
{
    hle_execute(&g_hle_audio);
}
