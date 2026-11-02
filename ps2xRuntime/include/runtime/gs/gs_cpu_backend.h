#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Software GS rasterizer.
//
// Rayman bring-up: drawing is deferred. The producer (the EE thread feeding GIF packets) only
// records primitives, CLUT loads, image uploads and local copies into "segments". A dispatcher
// thread executes closed segments in order: first the segment's pre-ops (uploads, CLUT loads,
// local copies) on one thread, then its primitives band-parallel (each worker owns a band of
// rows and walks every primitive of the segment in submission order). Hazards that would break
// that ordering (a primitive sampling what an earlier primitive of the same segment draws, an
// upload over a region an earlier primitive still needs, ...) close the segment first. Any
// producer-side VRAM read (local->host transfer, ReadVram, snapshots for presentation) waits
// until all queued work has executed. Set PS2X_GS_THREADS=0 to draw immediately instead.
class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();
    ~GSCpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Debug: capture every GS command between the next two presentations to out/gscapture.bin.
    static void RequestCapture();

    // Counters for the runtime's periodic status line.
    struct Stats
    {
        uint64_t prims = 0;
        uint64_t segments = 0;
        uint64_t hazardCloses = 0;
        uint64_t drains = 0;
        uint64_t drainWaitUs = 0;
        uint64_t spaceWaitUs = 0;
        uint64_t uploads = 0;
        uint64_t clutLoads = 0;
        uint32_t workers = 0;
    };
    static Stats ReadAndResetStats();

    struct Segment;
    struct PreOp;
    struct QueuedPrim;
    struct BandPool;
    struct PixelPipe;
    struct TexPipe;

private:
    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    // ---- producer side (m_mutex held) ----
    void ResetUnlocked();
    bool EngineEnabled();
    void StartEngineUnlocked();
    void StopEngine();
    Segment &OpenSegmentUnlocked();
    void CloseOpenSegmentUnlocked();
    void CloseForHazardUnlocked(std::unique_lock<std::mutex> &lock);
    void WaitForSpaceUnlocked(std::unique_lock<std::mutex> &lock);
    template <typename Fn>
    void WithQuiescentVram(Fn &&fn) const;

    // ---- execution side (dispatcher / band workers, or the producer in immediate mode) ----
    void DispatcherLoop();
    void RunPreOps(Segment &segment);
    void ExecuteBand(const Segment &segment, int band);
    void ExecuteUpload(const PreOp &op, const uint8_t *data);
    void ExecuteLocalCopy(const GSTransferCommand &transfer);
    void LoadClutInto(std::array<uint16_t, 512> &clut, const GSTex0Reg &tex0, const GSTexClutReg &texclut);

    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    bool ClearFramebufferUnlocked(const GSContext &context, uint32_t rgba);

    struct ClutRef
    {
        const uint16_t *data = nullptr;
        uint64_t generation = 0; // identifies the CLUT contents (instance, segment / version)
        uint32_t slot = 0;
    };
    void DrawPrimitive(const GSPrimitiveBatch &batch, const ClutRef &clut, int clipY0, int clipY1);
    void DrawSprite(const GSPrimitiveBatch &batch, const PixelPipe &pp, const TexPipe *tp, int clipY0, int clipY1);
    void DrawTriangle(const GSPrimitiveBatch &batch, const PixelPipe &pp, const TexPipe *tp, int clipY0, int clipY1);
    void DrawLine(const GSPrimitiveBatch &batch, const PixelPipe &pp, int clipY0, int clipY1);
    void SetupPixelPipe(PixelPipe &pp, const GSDrawState &state) const;
    void SetupTexPipe(TexPipe &tp, const GSDrawState &state, const ClutRef &clut) const;

    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    // capture (m_mutex held)
    void CaptureRecord(uint32_t kind, const void *a, uint32_t aSize, const void *b, uint32_t bSize);
    void CaptureFinishUnlocked(const char *reason);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    mutable std::mutex m_drainMutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint32_t, 2> m_clutCbp{};
    std::array<uint16_t, 512> m_execClut{}; // CLUT state as seen by the executor
    uint64_t m_instanceId = 0;
    uint64_t m_immClutVersion = 0;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;

    // ---- deferred engine ----
    int m_engineMode = -1; // -1 undecided, 0 immediate, >0 async with that many band workers
    std::unique_ptr<Segment> m_open;
    std::deque<std::unique_ptr<Segment>> m_fifo;      // closed segments; front is executing
    std::vector<std::unique_ptr<Segment>> m_freeSegments;
    uint64_t m_lastClosedId = 0;
    uint64_t m_completedId = 0;
    uint64_t m_activationLimit = ~0ull;
    size_t m_queuedPrims = 0;
    size_t m_queuedBytes = 0;
    bool m_engineStop = false;
    std::condition_variable m_dispatchCv;
    mutable std::condition_variable m_drainCv;
    std::condition_variable m_spaceCv;
    std::thread m_dispatcher;
    std::unique_ptr<BandPool> m_pool;

    // ---- capture ----
    std::FILE *m_captureFile = nullptr;
    uint32_t m_captureRecords = 0;
    uint32_t m_captureFinishes = 0;
};
