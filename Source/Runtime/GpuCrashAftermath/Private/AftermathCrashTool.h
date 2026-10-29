#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/GpuTools.h"
#include "RHI/RhiPipeline.h"
#include "RHIVulkan/VulkanApi.h"
#include "String/String.h"
#include "AftermathCrashTool.gen.h"

namespace Marlin
{
/// How much Aftermath records; more costs GPU and driver time.
MENUM()
enum class AftermathLevel : uint8
{
    /// Aftermath is not started.
    Off,
    /// Crash dumps only, with the faulting shaders named but not kept.
    Minimal,
    /// Also driver resource tracking and shader debug information, for page faults and shader lines.
    Standard,
    /// Also a checkpoint after every command and extra shader error reporting; slows the GPU noticeably.
    Full,
};

/// Writes GPU crash dumps to /User/Crashes on NVIDIA GPUs and decodes them into the crash report: the device status,
/// the page fault and its resource, the shaders that were running and their faulted warps, named by the pipelines
/// that made them. With a capture tool injected it records crash dumps only, so it disturbs neither the capture nor
/// its timings.
MCLASS(Config = "Engine.GpuCrash.Aftermath")
class GPUCRASHAFTERMATH_API AftermathCrashTool : public IGpuCrashTool
{
    MGENERATED_BODY()
public:
    /// `-Aftermath=Off|Minimal|Standard|Full` overrides it.
    MPROPERTY()
    static AftermathLevel Level;

    [[nodiscard]] Name GetName() const override;
    [[nodiscard]] bool SupportsBackend(Name backend) const override;
    bool Initialize(IRhiBackend &backend, const IGpuCaptureTool *captureTool) override;
    void Shutdown() override;
    void OnDeviceCreated(RhiDevice &device) override;
    void OnDeviceDestroying(RhiDevice &device) override;
    void OnPipelineCreated(Span<const RhiShader> shaders, StringView debugName) override;
    void OnDeviceLost(RhiDevice &device, StringWriter &report) override;

    /// From Aftermath's thread: writes the dump to *path* and keeps it for decoding.
    void RecordDump(StringView path, Span<const uint8> dump);
    /// From Aftermath's thread: shader debug information, kept in memory for decoding only.
    void RecordShaderDebugInfo(uint64 id0, uint64 id1, Span<const uint8> info);

    /// A pipeline stage, by its Aftermath shader hash: its names, and its SPIR-V from Standard up.
    struct Shader
    {
        String Pipeline;
        String EntryPoint;
        RhiShaderStage Stage = RhiShaderStage::None;
        /// Empty at Minimal.
        List<uint8> Code;
    };

    /// Aftermath's debug information of a shader, by its identifier.
    struct ShaderDebugInfo
    {
        uint64 Id[2] = {};
        List<uint8> Data;
    };

    /// For the decoder's lookups; null if unknown. Decode calls them with the lock held.
    [[nodiscard]] const Shader *FindShader(uint64 hash) const;
    [[nodiscard]] const ShaderDebugInfo *FindShaderDebugInfo(uint64 id0, uint64 id1) const;

    /// Decodes the recorded dump into *summary*, and attaches the decoder's full JSON next to the dump.
    void Decode(StringWriter &summary);

private:
    bool _enabled = false;
    AftermathLevel _level = AftermathLevel::Off;
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV _features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV, nullptr, VK_FALSE};
    VkDeviceDiagnosticsConfigCreateInfoNV _config{VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV, nullptr,
                                                  0};
    SpinLock _lock;
    /// Every pipeline's shaders since startup, so a dump can be decoded after the pipelines are gone.
    HashMap<uint64, Shader> _shaders;
    List<ShaderDebugInfo> _shaderDebugInfos;
    /// The Vfs path of the dump, and the dump itself; empty until Aftermath writes one.
    String _dumpPath;
    List<uint8> _dump;
};
} // namespace Marlin
