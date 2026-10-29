#include "AftermathCrashTool.h"

#include "Concurrency/ScopeLock.h"
#include "CrashReport/CrashReportClient.h"
#include "Diagnostics/Log.h"
#include "Engine/Engine.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonValue.h"
#include "Misc/CommandLine.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiLog.h"
#include "RHIVulkan/VulkanBackend.h"
#include "Reflection/Reflection.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

#include <vulkan/vulkan.h>

#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

namespace Marlin
{
AftermathLevel AftermathCrashTool::Level = AftermathLevel::Standard;

namespace
{
constexpr uint32 NvidiaVendorId = 0x10DE;
constexpr uint32 DumpWaitMilliseconds = 10000;
// RECOVERY-DECISION D5: a lost device gets 3 s for Aftermath to start collecting before the report goes without it.
constexpr uint32 NotStartedWaitMilliseconds = 3000;

StringView ToBytes(Span<const uint8> data)
{
    return StringView(reinterpret_cast<const char *>(data.Data()), data.Count());
}

/// Writes *contents* to the Vfs *path* and returns its file-system path, for the crash report; empty on failure.
String WriteCrashFile(StringView path, StringView contents)
{
    StringBuilder<512> physical;
    if (!Vfs::WriteAll(path, contents) || !Vfs::GetPhysicalPath(path, physical))
    {
        return {};
    }
    return String(physical.AsView());
}

void GFSDK_AFTERMATH_CALL OnCrashDump(const void *dump, const uint32 size, void *context)
{
    StringBuilder<256> path;
    FormatTo(path, "/User/Crashes/GpuCrash-{0}.nv-gpudmp", PlatformTime::UtcNow());
    static_cast<AftermathCrashTool *>(context)->RecordDump(
        path.AsView(), Span<const uint8>(static_cast<const uint8 *>(dump), size));
}

void GFSDK_AFTERMATH_CALL OnShaderDebugInfo(const void *info, const uint32 size, void *context)
{
    GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier{};
    if (GFSDK_Aftermath_SUCCEED(
            GFSDK_Aftermath_GetShaderDebugInfoIdentifier(GFSDK_Aftermath_Version_API, info, size, &identifier)))
    {
        static_cast<AftermathCrashTool *>(context)->RecordShaderDebugInfo(
            identifier.id[0], identifier.id[1], Span<const uint8>(static_cast<const uint8 *>(info), size));
    }
}

void GFSDK_AFTERMATH_CALL OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription add, void * /*context*/)
{
    const EngineEnvironment &environment = Engine::GetEnvironment();
    const String &name =
        !environment.Product.ProductName.IsEmpty() ? environment.Product.ProductName : environment.TargetName;
    add(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, name.CStr());
    const String &version =
        !environment.Product.Version.IsEmpty() ? environment.Product.Version : environment.EngineVersion;
    add(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, version.CStr());
}

StringView GetStatusName(GFSDK_Aftermath_Device_Status status)
{
    switch (status)
    {
    case GFSDK_Aftermath_Device_Status_Active:
        return "active";
    case GFSDK_Aftermath_Device_Status_Timeout:
        return "timeout (a hang)";
    case GFSDK_Aftermath_Device_Status_OutOfMemory:
        return "out of memory";
    case GFSDK_Aftermath_Device_Status_PageFault:
        return "page fault";
    case GFSDK_Aftermath_Device_Status_Stopped:
        return "stopped";
    case GFSDK_Aftermath_Device_Status_Reset:
        return "reset";
    case GFSDK_Aftermath_Device_Status_DmaFault:
        return "DMA fault";
    case GFSDK_Aftermath_Device_Status_DeviceRemovedNoGpuFault:
        return "removed without a GPU fault";
    default:
        return "unknown";
    }
}

StringView GetFaultName(GFSDK_Aftermath_FaultType fault)
{
    switch (fault)
    {
    case GFSDK_Aftermath_FaultType_AddressTranslationError:
        return "address translation error";
    case GFSDK_Aftermath_FaultType_IllegalAccessError:
        return "illegal access";
    default:
        return "unknown fault";
    }
}

StringView GetAccessName(GFSDK_Aftermath_AccessType access)
{
    switch (access)
    {
    case GFSDK_Aftermath_AccessType_Read:
        return "read";
    case GFSDK_Aftermath_AccessType_Write:
        return "write";
    case GFSDK_Aftermath_AccessType_Atomic:
        return "atomic";
    default:
        return "access";
    }
}

StringView GetEngineName(GFSDK_Aftermath_Engine engine)
{
    switch (engine)
    {
    case GFSDK_Aftermath_Engine_Graphics:
        return "graphics";
    case GFSDK_Aftermath_Engine_GraphicsCompute:
        return "graphics/compute";
    case GFSDK_Aftermath_Engine_Display:
        return "display";
    case GFSDK_Aftermath_Engine_CopyEngine:
        return "copy";
    case GFSDK_Aftermath_Engine_VideoDecoder:
        return "video decoder";
    case GFSDK_Aftermath_Engine_VideoEncoder:
        return "video encoder";
    case GFSDK_Aftermath_Engine_Other:
        return "other";
    default:
        return "unknown";
    }
}

StringView GetClientName(GFSDK_Aftermath_Client client)
{
    switch (client)
    {
    case GFSDK_Aftermath_Client_HostInterface:
        return "host interface";
    case GFSDK_Aftermath_Client_FrontEnd:
        return "front end";
    case GFSDK_Aftermath_Client_PrimitiveDistributor:
        return "primitive distributor";
    case GFSDK_Aftermath_Client_GraphicsProcessingCluster:
        return "graphics processing cluster";
    case GFSDK_Aftermath_Client_PolymorphEngine:
        return "polymorph engine";
    case GFSDK_Aftermath_Client_RasterEngine:
        return "raster engine";
    case GFSDK_Aftermath_Client_Rasterizer2D:
        return "2D rasterizer";
    case GFSDK_Aftermath_Client_RenderOutputUnit:
        return "render output unit";
    case GFSDK_Aftermath_Client_TextureProcessingCluster:
        return "texture processing cluster";
    case GFSDK_Aftermath_Client_CopyEngine:
        return "copy engine";
    case GFSDK_Aftermath_Client_VideoDecoder:
        return "video decoder";
    case GFSDK_Aftermath_Client_VideoEncoder:
        return "video encoder";
    default:
        return "unknown client";
    }
}

StringView GetShaderTypeName(GFSDK_Aftermath_ShaderType type)
{
    switch (type)
    {
    case GFSDK_Aftermath_ShaderType_Vertex:
        return "vertex";
    case GFSDK_Aftermath_ShaderType_Pixel:
        return "pixel";
    case GFSDK_Aftermath_ShaderType_Compute:
        return "compute";
    case GFSDK_Aftermath_ShaderType_RayTracing_RayGeneration:
        return "ray generation";
    case GFSDK_Aftermath_ShaderType_RayTracing_Miss:
        return "miss";
    case GFSDK_Aftermath_ShaderType_RayTracing_Intersection:
        return "intersection";
    case GFSDK_Aftermath_ShaderType_RayTracing_AnyHit:
        return "any hit";
    case GFSDK_Aftermath_ShaderType_RayTracing_ClosestHit:
        return "closest hit";
    case GFSDK_Aftermath_ShaderType_RayTracing_Callable:
        return "callable";
    case GFSDK_Aftermath_ShaderType_RayTracing_Internal:
        return "internal ray tracing";
    case GFSDK_Aftermath_ShaderType_Mesh:
        return "mesh";
    case GFSDK_Aftermath_ShaderType_Task:
        return "task";
    default:
        return "unknown";
    }
}

/// An active shader's Aftermath hash and the name the summary gives it.
struct ShaderLabel
{
    uint64 Hash = 0;
    String Name;
};

/// Splits a warp address such as `0x1a2b3c4d5e6f7081+0x1c0` into its shader part and its offset.
void SplitAddress(StringView address, StringView &name, StringView &offset)
{
    const usize plus = address.IndexOf('+');
    name = plus == StringView::NotFound ? address.Trim() : address.Left(plus).Trim();
    offset = plus == StringView::NotFound ? StringView() : address.RemoveStart(plus + 1).Trim();
}

/// Writes one line per warp the decoder's JSON lists, with shader hashes replaced by the summary's labels.
/// RECOVERY-DECISION D6: the JSON layout is undocumented, so every object below a member whose key mentions warps is
/// a warp; its scalar members are written, at most 64 warps.
void WriteWarps(StringView json, Span<const ShaderLabel> labels, StringWriter &summary)
{
    JsonValue root;
    JsonError error;
    if (!Json::Parse(json, root, error))
    {
        FormatTo(summary, "The decoded dump is not JSON ({0}:{1}: {2})\n", error.Line, error.Column, error.Message);
        return;
    }
    const auto labelOf = [&](StringView text) -> StringView {
        StringView digits = text.StartsWith("0x") || text.StartsWith("0X") ? text.RemoveStart(2) : text;
        uint64 hash = 0;
        if (digits.IsEmpty() || !TryParse(digits, hash, 16))
        {
            return {};
        }
        for (const ShaderLabel &label : labels)
        {
            if (label.Hash == hash)
            {
                return label.Name.AsView();
            }
        }
        return {};
    };
    struct Pending
    {
        const JsonValue *Value;
        bool InWarps;
    };
    List<Pending> pending;
    pending.Add({&root, false});
    uint32 written = 0;
    constexpr uint32 MaxWarps = 64;
    while (!pending.IsEmpty() && written < MaxWarps)
    {
        const Pending next = pending.Pop();
        if (next.Value->IsArray())
        {
            for (const JsonValue &element : next.Value->GetElements())
            {
                pending.Add({&element, next.InWarps});
            }
            continue;
        }
        if (!next.Value->IsObject())
        {
            continue;
        }
        StringBuilder<512> line;
        for (const JsonMember &member : next.Value->GetMembers())
        {
            const StringView key = member.Key.AsView();
            if (member.Value.IsObject() || member.Value.IsArray())
            {
                pending.Add({&member.Value, next.InWarps || key.Contains("Warp") || key.Contains("warp")});
                continue;
            }
            if (!next.InWarps)
            {
                continue;
            }
            const String value = member.Value.IsString() ? String(member.Value.AsString()) : member.Value.ToString();
            StringView name;
            StringView offset;
            SplitAddress(value.AsView(), name, offset);
            const StringView label = labelOf(name);
            FormatTo(line, "{0}{1}=", line.Length() > 0 ? " " : "", key);
            if (label.IsEmpty())
            {
                line.Append(value.AsView());
            }
            else
            {
                FormatTo(line, "{0}{1}{2}", label, offset.IsEmpty() ? "" : "+", offset);
            }
        }
        if (line.Length() > 0)
        {
            FormatTo(summary, "  warp: {0}\n", line.AsView());
            ++written;
        }
    }
    if (written == 0)
    {
        summary.Append("  The dump holds no warp state.\n");
    }
}

/// The decoder's lookups, which hand it the shaders and debug information the tool kept.
struct DecoderLookups
{
    static void GFSDK_AFTERMATH_CALL ShaderDebugInfo(const GFSDK_Aftermath_ShaderDebugInfoIdentifier *identifier,
                                                     PFN_GFSDK_Aftermath_SetData set, void *context);
    static void GFSDK_AFTERMATH_CALL ShaderBinary(const GFSDK_Aftermath_ShaderBinaryHash *hash,
                                                  PFN_GFSDK_Aftermath_SetData set, void *context);
};
} // namespace

Name AftermathCrashTool::GetName() const
{
    return Name("Aftermath");
}

bool AftermathCrashTool::SupportsBackend(Name backend) const
{
    return backend.ToView() == StringView("Vulkan");
}

bool AftermathCrashTool::Initialize(IRhiBackend &backend, const IGpuCaptureTool *captureTool)
{
    AftermathLevel level = Level;
    StringView value;
    // Shipping keeps the configured level: players cannot turn on the costlier ones.
    if (!MARLIN_BUILD_SHIPPING && CommandLine::TryGetValue("Aftermath", value) &&
        !Reflection::ParseValue(value, TypeOf<AftermathLevel>(), &level))
    {
        MARLIN_LOG(LogRhi, Warning, "-Aftermath={0} is not Off, Minimal, Standard or Full", value);
    }
    if (level == AftermathLevel::Off)
    {
        return false;
    }
    const Span<const RhiAdapterInfo> adapters = backend.GetAdapters();
    if (adapters.IsEmpty() || adapters[0].VendorId != NvidiaVendorId)
    {
        return false;
    }
    if (captureTool != nullptr && level > AftermathLevel::Minimal)
    {
        MARLIN_LOG(LogRhi, Info, "Aftermath records crash dumps only while {0} is injected", captureTool->GetName());
        level = AftermathLevel::Minimal;
    }

    auto &vulkan = static_cast<VulkanBackend &>(backend);
    if (!vulkan.RequestDeviceExtension(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME))
    {
        return false;
    }
    _config.flags = 0;
    if (level >= AftermathLevel::Standard)
    {
        _config.flags |= VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV |
                         VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV;
    }
    if (level >= AftermathLevel::Full && vulkan.RequestDeviceExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
    {
        _config.flags |= VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV |
                         VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
    }

    const GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API, GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks, &OnCrashDump,
        level >= AftermathLevel::Standard ? &OnShaderDebugInfo : nullptr, &OnDescription, nullptr, this);
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        MARLIN_LOG(LogRhi, Warning, "Aftermath did not start ({0})", static_cast<uint32>(result));
        return false;
    }
    _features.diagnosticsConfig = VK_TRUE;
    vulkan.ChainDeviceCreateInfo(reinterpret_cast<VkBaseOutStructure *>(&_features));
    vulkan.ChainDeviceCreateInfo(reinterpret_cast<VkBaseOutStructure *>(&_config));
    _enabled = true;
    _level = level;
    MARLIN_LOG(LogRhi, Info, "Aftermath GPU crash dumps enabled");
    return true;
}

void AftermathCrashTool::Shutdown()
{
    if (_enabled)
    {
        GFSDK_Aftermath_DisableGpuCrashDumps();
        _enabled = false;
    }
}

void AftermathCrashTool::OnDeviceCreated(RhiDevice & /*device*/)
{
}

void AftermathCrashTool::OnDeviceDestroying(RhiDevice & /*device*/)
{
}

void AftermathCrashTool::OnPipelineCreated(Span<const RhiShader> shaders, StringView debugName)
{
    if (!_enabled)
    {
        return;
    }
    for (const RhiShader &shader : shaders)
    {
        GFSDK_Aftermath_SpirvCode code{};
        code.pData = shader.Code.Data();
        code.size = static_cast<uint32>(shader.Code.Count());
        GFSDK_Aftermath_ShaderBinaryHash hash{};
        if (!GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GetShaderHashSpirv(GFSDK_Aftermath_Version_API, &code, &hash)))
        {
            continue;
        }
        const ScopeLock lock(_lock);
        if (_shaders.Contains(hash.hash))
        {
            continue;
        }
        Shader entry;
        entry.Pipeline = String(debugName);
        entry.EntryPoint = String(shader.EntryPoint);
        entry.Stage = shader.Stage;
        // RECOVERY-DECISION D7: Minimal keeps only the names, Standard and Full also the SPIR-V for the decoder.
        if (_level >= AftermathLevel::Standard)
        {
            entry.Code.AddRange(shader.Code);
        }
        _shaders.Add(hash.hash, static_cast<Shader &&>(entry));
    }
}

void AftermathCrashTool::OnDeviceLost(RhiDevice & /*device*/, StringWriter &report)
{
    if (!_enabled)
    {
        return;
    }
    GFSDK_Aftermath_CrashDump_Status status = GFSDK_Aftermath_CrashDump_Status_Unknown;
    const uint64 start = PlatformTime::Ticks();
    const uint64 ticksPerMillisecond = PlatformTime::TicksPerSecond() / 1000;
    GFSDK_Aftermath_GetCrashDumpStatus(&status);
    while (status != GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed &&
           status != GFSDK_Aftermath_CrashDump_Status_Finished && status != GFSDK_Aftermath_CrashDump_Status_Unknown)
    {
        const uint64 waited = (PlatformTime::Ticks() - start) / ticksPerMillisecond;
        if (waited >= DumpWaitMilliseconds ||
            (status == GFSDK_Aftermath_CrashDump_Status_NotStarted && waited >= NotStartedWaitMilliseconds))
        {
            break;
        }
        PlatformThread::Sleep(50);
        GFSDK_Aftermath_GetCrashDumpStatus(&status);
    }
    const ScopeLock lock(_lock);
    if (_dumpPath.IsEmpty())
    {
        FormatTo(report, "Aftermath wrote no GPU crash dump (status {0})\n", static_cast<uint32>(status));
        return;
    }
    StringBuilder<512> physical;
    Vfs::GetPhysicalPath(_dumpPath.AsView(), physical);
    FormatTo(report, "Aftermath GPU crash dump: {0}\n", physical.AsView());
    Decode(report);
}

void AftermathCrashTool::RecordDump(StringView path, Span<const uint8> dump)
{
    const String physical = WriteCrashFile(path, ToBytes(dump));
    if (!physical.IsEmpty())
    {
        CrashReport::AddAttachment(physical.AsView());
    }
    const ScopeLock lock(_lock);
    _dumpPath = String(path);
    _dump.Clear();
    _dump.AddRange(dump);
}

void AftermathCrashTool::RecordShaderDebugInfo(uint64 id0, uint64 id1, Span<const uint8> info)
{
    const ScopeLock lock(_lock);
    // RECOVERY-DECISION D8: debug information embeds the shader source, so it stays in memory and never ships.
    ShaderDebugInfo &entry = _shaderDebugInfos.AddDefaulted();
    entry.Id[0] = id0;
    entry.Id[1] = id1;
    entry.Data.AddRange(info);
}

const AftermathCrashTool::Shader *AftermathCrashTool::FindShader(uint64 hash) const
{
    return _shaders.Find(hash);
}

const AftermathCrashTool::ShaderDebugInfo *AftermathCrashTool::FindShaderDebugInfo(uint64 id0, uint64 id1) const
{
    const usize index = _shaderDebugInfos.FindIndex(
        [&](const ShaderDebugInfo &entry) { return entry.Id[0] == id0 && entry.Id[1] == id1; });
    return index < _shaderDebugInfos.Count() ? &_shaderDebugInfos[index] : nullptr;
}

void GFSDK_AFTERMATH_CALL DecoderLookups::ShaderDebugInfo(const GFSDK_Aftermath_ShaderDebugInfoIdentifier *identifier,
                                                          PFN_GFSDK_Aftermath_SetData set, void *context)
{
    const auto *tool = static_cast<const AftermathCrashTool *>(context);
    const AftermathCrashTool::ShaderDebugInfo *info = tool->FindShaderDebugInfo(identifier->id[0], identifier->id[1]);
    if (info != nullptr)
    {
        set(info->Data.Data(), static_cast<uint32>(info->Data.Count()));
    }
}

void GFSDK_AFTERMATH_CALL DecoderLookups::ShaderBinary(const GFSDK_Aftermath_ShaderBinaryHash *hash,
                                                       PFN_GFSDK_Aftermath_SetData set, void *context)
{
    const auto *tool = static_cast<const AftermathCrashTool *>(context);
    const AftermathCrashTool::Shader *shader = tool->FindShader(hash->hash);
    if (shader != nullptr && !shader->Code.IsEmpty())
    {
        set(shader->Code.Data(), static_cast<uint32>(shader->Code.Count()));
    }
}

void AftermathCrashTool::Decode(StringWriter &summary)
{
    GFSDK_Aftermath_GpuCrashDump_Decoder decoder{};
    if (_dump.IsEmpty() ||
        !GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
            GFSDK_Aftermath_Version_API, _dump.Data(), static_cast<uint32>(_dump.Count()), &decoder)))
    {
        summary.Append("Aftermath cannot decode the GPU crash dump\n");
        return;
    }

    GFSDK_Aftermath_GpuCrashDump_DeviceInfo device{};
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo(decoder, &device)))
    {
        FormatTo(summary, "GPU status: {0}{1}{2}\n", GetStatusName(device.status),
                 device.adapterReset ? ", adapter reset" : "", device.engineReset ? ", engine reset" : "");
    }

    GFSDK_Aftermath_GpuCrashDump_PageFaultInfo fault{};
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetPageFaultInfo(decoder, &fault)))
    {
        FormatTo(summary, "Page fault at 0x{0:x16}: {1} on a {2} by the {3} engine ({4})\n", fault.faultingGpuVA,
                 GetFaultName(fault.faultType), GetAccessName(fault.accessType), GetEngineName(fault.engine),
                 GetClientName(fault.client));
        if (fault.resourceInfoCount > 0)
        {
            List<GFSDK_Aftermath_GpuCrashDump_ResourceInfo> resources;
            resources.Resize(fault.resourceInfoCount);
            if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetPageFaultResourceInfo(
                    decoder, fault.resourceInfoCount, resources.Data())))
            {
                for (const GFSDK_Aftermath_GpuCrashDump_ResourceInfo &resource : resources)
                {
                    FormatTo(summary, "  resource {0}: 0x{1:x16}, {2} bytes, {3}x{4}x{5}, {6} mips{7}\n",
                             resource.debugName[0] != 0 ? resource.debugName : "(unnamed)", resource.gpuVa,
                             resource.size, resource.width, resource.height, resource.depth, resource.mipLevels,
                             resource.bWasDestroyed ? ", already destroyed" : "");
                }
            }
        }
    }

    List<ShaderLabel> labels;
    uint32 shaderCount = 0;
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount(decoder, &shaderCount)) &&
        shaderCount > 0)
    {
        List<GFSDK_Aftermath_GpuCrashDump_ShaderInfo> shaders;
        shaders.Resize(shaderCount);
        if (GFSDK_Aftermath_SUCCEED(
                GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo(decoder, shaderCount, shaders.Data())))
        {
            summary.Append("Active shaders:\n");
            for (const GFSDK_Aftermath_GpuCrashDump_ShaderInfo &info : shaders)
            {
                ShaderLabel &label = labels.AddDefaulted();
                label.Hash = info.shaderHash;
                StringBuilder<256> name;
                if (const Shader *shader = FindShader(info.shaderHash))
                {
                    FormatTo(name, "{0} {1}", shader->Pipeline, shader->EntryPoint);
                }
                else if (info.isInternal)
                {
                    name.Append("(driver internal)");
                }
                else
                {
                    FormatTo(name, "0x{0:x16}", info.shaderHash);
                }
                label.Name = String(name.AsView());
                FormatTo(summary, "  {0} shader {1}\n", GetShaderTypeName(info.shaderType), label.Name);
            }
        }
    }

    uint32 jsonSize = 0;
    const uint32 flags = GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO;
    if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GenerateJSON(
            decoder, flags, GFSDK_Aftermath_GpuCrashDumpFormatterFlags_UTF8_OUTPUT, &DecoderLookups::ShaderDebugInfo,
            &DecoderLookups::ShaderBinary, nullptr, this, &jsonSize)) &&
        jsonSize > 0)
    {
        List<char> json;
        json.Resize(jsonSize);
        if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetJSON(decoder, jsonSize, json.Data())))
        {
            const StringView text(json.Data(), jsonSize > 0 && json[jsonSize - 1] == 0 ? jsonSize - 1 : jsonSize);
            summary.Append("Faulted warps:\n");
            WriteWarps(text, labels, summary);
            StringBuilder<256> jsonPath;
            FormatTo(jsonPath, "{0}.json", _dumpPath);
            const String physical = WriteCrashFile(jsonPath.AsView(), text);
            if (!physical.IsEmpty())
            {
                CrashReport::AddAttachment(physical.AsView());
                FormatTo(summary, "Decoded GPU crash dump: {0}\n", physical);
            }
        }
    }
    GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
}
} // namespace Marlin
