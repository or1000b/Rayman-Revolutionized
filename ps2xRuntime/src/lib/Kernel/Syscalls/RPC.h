#pragma once

#include "ps2_syscalls.h"

namespace ps2_syscalls
{
    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // IOP -> EE RPC: requests the IOP posted to EE-registered servers, served by the EE's
    // sceSifGetNextRequest/sceSifExecRequest loop.
    uint32_t takeIopToEeRequest(uint8_t *rdram, uint32_t queuePtr);
    void execIopToEeRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t serverPtr);
}

// Installs the IOP -> EE RPC hooks into the IOP emulator (called once the IOP subsystem exists).
void ps2xInstallIopToEeRpcHooks(PS2Runtime *runtime);
int ps2xIopToEeRpcStatsLine(char *buffer, int size);
