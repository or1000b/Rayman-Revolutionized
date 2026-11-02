#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

// IOP -> EE SIF RPC. The EE side (request queue, sceSifGetNextRequest/sceSifExecRequest) lives in
// the runtime; it installs these hooks at start-up. Plain C so the two libraries need no shared header.
extern "C"
{
    struct Ps2xEeRpcHooks
    {
        // Returns 1 when the EE registered a server for `sid` (fills its EE-side descriptor fields).
        int (*lookup)(uint32_t sid, uint32_t *server, uint32_t *buffer, uint32_t *cbuffer);
        // Queues a call to an EE server and wakes its queue thread. Returns 0 when accepted.
        int (*post)(uint32_t sid, uint32_t function, const uint8_t *send, uint32_t sendSize,
                    uint32_t receiveSize, uint64_t token);
    };
    void ps2xIopSetEeRpcHooks(const Ps2xEeRpcHooks *hooks);
    // Called by the runtime when the EE server function returned: copies the reply to the IOP
    // receive buffer and releases the waiting IOP thread.
    void ps2xIopEeRpcCompleted(uint64_t token, const uint8_t *reply, uint32_t size);
}

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;
        ~IopRpcBridge();
        IopRpcBridge(const IopRpcBridge &) = delete;
        IopRpcBridge &operator=(const IopRpcBridge &) = delete;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

        // IOP -> EE calls: reply delivery and timeouts for calls the EE never answers.
        void completeEeCall(uint64_t token, const uint8_t *reply, uint32_t size);
        void serviceEeCalls(uint64_t nowCycle);

    private:
        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        struct EeClient
        {
            uint32_t sid = 0;
            uint32_t eeServer = 0;
            bool busy = false;
            uint64_t token = 0;
            uint32_t receiveAddress = 0;
            uint32_t receiveSize = 0;
            int waitingThread = 0;
            uint64_t deadline = 0;
        };

        void releaseEeClient(EeClient &client);

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        std::unordered_map<uint32_t, EeClient> m_eeClients; // keyed by the IOP client descriptor
        uint64_t m_nextEeToken = 1u;
        uint64_t m_nowCycle = 0u;
        uint32_t m_eeCallTimeouts = 0u;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
    };
}
