#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "../iop_emulator_const.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
    Ps2xEeRpcHooks g_eeRpcHooks{};
    ps2x::iop::detail::IopRpcBridge *g_eeRpcBridge = nullptr;

    // sceSifClientData (IOP side): 16-byte header, then command, buff, cbuff, end_function, end_param, server.
    constexpr uint32_t kClientCommand = 0x10u;
    constexpr uint32_t kClientBuffer = 0x14u;
    constexpr uint32_t kClientCBuffer = 0x18u;
    constexpr uint32_t kClientServer = 0x24u;
    // A synchronous IOP -> EE call that the EE never answers must not freeze the calling IOP thread.
    constexpr uint64_t kEeCallTimeoutCycles = kIopClockHz; // 1 s of IOP time
    constexpr uint32_t kMaxEeCallPayload = 0x10000u;
}

extern "C" void ps2xIopSetEeRpcHooks(const Ps2xEeRpcHooks *hooks)
{
    g_eeRpcHooks = hooks ? *hooks : Ps2xEeRpcHooks{};
}

extern "C" void ps2xIopEeRpcCompleted(uint64_t token, const uint8_t *reply, uint32_t size)
{
    if (g_eeRpcBridge)
        g_eeRpcBridge->completeEeCall(token, reply, size);
}

namespace ps2x::iop::detail
{
    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
        g_eeRpcBridge = this;
    }

    IopRpcBridge::~IopRpcBridge()
    {
        if (g_eeRpcBridge == this)
            g_eeRpcBridge = nullptr;
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_eeClients.clear();
        m_nextDmaId = 1u;
        m_sifInitialized = false;
    }

    void IopRpcBridge::releaseEeClient(EeClient &client)
    {
        client.busy = false;
        client.token = 0u;
        if (client.waitingThread > 0)
            (void)m_kernel.wakeSleeping(client.waitingThread);
        client.waitingThread = 0;
    }

    void IopRpcBridge::completeEeCall(uint64_t token, const uint8_t *reply, uint32_t size)
    {
        for (auto &[descriptor, client] : m_eeClients)
        {
            if (!client.busy || client.token != token)
                continue;
            if (client.receiveAddress != 0u && client.receiveSize != 0u && reply != nullptr)
            {
                const uint32_t copy = std::min(client.receiveSize, size);
                if (copy != 0u && m_memory.ownsRamRange(client.receiveAddress, copy))
                    (void)m_memory.writeRam(client.receiveAddress, reply, copy);
            }
            releaseEeClient(client);
            return;
        }
    }

    void IopRpcBridge::serviceEeCalls(uint64_t nowCycle)
    {
        m_nowCycle = nowCycle;
        for (auto &[descriptor, client] : m_eeClients)
        {
            if (!client.busy || client.waitingThread <= 0 || nowCycle < client.deadline)
                continue;
            if (m_eeCallTimeouts++ < 8u)
            {
                std::ostringstream out;
                out << "[IOP] IOP->EE RPC sid=0x" << std::hex << client.sid << " got no EE reply; releasing thread " << std::dec
                    << client.waitingThread;
                m_host.log(LogLevel::Warning, out.str());
            }
            releaseEeClient(client);
        }
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        {
            constexpr uint32_t kDescriptorSize = 16u;
            constexpr uint32_t kMaxDescriptors = 32u;
            const uint32_t descriptorAddress = cpu.gpr[4];
            const uint32_t descriptorCount = cpu.gpr[5];
            if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
            {
                setV0(0u);
                return true;
            }

            struct PendingTransfer
            {
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
            };

            std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
            const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
            if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
            {
                setV0(0u);
                return true;
            }

            std::array<PendingTransfer, kMaxDescriptors> pending{};
            uint32_t pendingCount = 0u;
            uint32_t largestTransfer = 0u;
            for (uint32_t i = 0u; i < descriptorCount; ++i)
            {
                const uint32_t source = descriptorWords[i * 4u + 0u];
                const uint32_t destination = descriptorWords[i * 4u + 1u];
                const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
                if (signedSize <= 0)
                    continue;

                const uint32_t size = static_cast<uint32_t>(signedSize);
                if (!m_memory.ownsRamRange(source, size))
                {
                    setV0(0u);
                    return true;
                }
                pending[pendingCount++] = {source, destination, size};
                largestTransfer = std::max(largestTransfer, size);
            }

            // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
            // destinations before committing any write so a bad chain cannot
            // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
            // TODO check later
            std::vector<uint8_t> scratch(largestTransfer);
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                m_nextDmaId = 1u;
            }
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 14: // InitRpc
            setV0(0);
            return true;
        case 15: // sceSifBindRpc: bind an IOP client to a server the EE registered
        {
            const uint32_t descriptor = cpu.gpr[4];
            const uint32_t sid = cpu.gpr[5];
            uint32_t server = 0u, buffer = 0u, cbuffer = 0u;
            if (descriptor == 0u || !m_memory.ownsRamRange(descriptor, 0x28u))
            {
                setV0(static_cast<uint32_t>(-1));
                return true;
            }
            if (g_eeRpcHooks.lookup && g_eeRpcHooks.lookup(sid, &server, &buffer, &cbuffer) != 0 && server != 0u)
            {
                m_memory.write32(descriptor + kClientCommand, 0u);
                m_memory.write32(descriptor + kClientBuffer, buffer);
                m_memory.write32(descriptor + kClientCBuffer, cbuffer);
                m_memory.write32(descriptor + kClientServer, server);
                EeClient &client = m_eeClients[descriptor];
                client.sid = sid;
                client.eeServer = server;
            }
            else
            {
                // cd->server == 0 is the SDK's "not registered yet, retry" signal.
                m_memory.write32(descriptor + kClientServer, 0u);
            }
            setV0(0);
            return true;
        }
        case 16: // sceSifCallRpc (IOP -> EE)
        {
            const uint32_t descriptor = cpu.gpr[4];
            const uint32_t function = cpu.gpr[5];
            const uint32_t mode = cpu.gpr[6];
            const uint32_t send = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t sendSize = m_memory.read32(stackPointer + 16u);
            const uint32_t receive = m_memory.read32(stackPointer + 20u);
            const uint32_t receiveSize = m_memory.read32(stackPointer + 24u);
            const auto clientIt = m_eeClients.find(descriptor);
            if (clientIt == m_eeClients.end() || !g_eeRpcHooks.post)
            {
                setV0(static_cast<uint32_t>(-1));
                return true;
            }
            EeClient &client = clientIt->second;
            std::vector<uint8_t> payload(std::min(sendSize, kMaxEeCallPayload));
            if (!payload.empty() && (send == 0u || !m_memory.readRam(send, payload.data(), payload.size())))
                payload.clear();
            const uint64_t token = m_nextEeToken++;
            client.busy = true;
            client.token = token;
            client.receiveAddress = receive;
            client.receiveSize = receiveSize;
            client.waitingThread = 0;
            m_memory.write32(descriptor + kClientCommand, function);
            if (g_eeRpcHooks.post(client.sid, function, payload.data(), static_cast<uint32_t>(payload.size()), receiveSize, token) != 0)
            {
                client.busy = false;
                client.token = 0u;
                setV0(static_cast<uint32_t>(-1));
                return true;
            }
            if ((mode & 1u) == 0u && client.busy) // synchronous: block until the EE reply arrives
            {
                const int thread = m_kernel.currentThreadId();
                if (thread > 0)
                {
                    client.waitingThread = thread;
                    client.deadline = m_nowCycle + kEeCallTimeoutCycles;
                    m_kernel.sleepCurrent(cpu);
                }
            }
            setV0(0);
            return true;
        }
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18: // sceSifCheckStatRpc: 1 while an IOP -> EE call is still in flight
        {
            const auto clientIt = m_eeClients.find(cpu.gpr[4]);
            setV0(clientIt != m_eeClients.end() && clientIt->second.busy ? 1u : 0u);
            return true;
        }
        case 19: // SetRpcQueue
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23:
            setV0(0);
            return true;
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t returnPointer = executor.executeGuestFunction(server.function,
                                                               request.function,
                                                               server.buffer,
                                                               request.send.size,
                                                               0u,
                                                               server.gp);
        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
