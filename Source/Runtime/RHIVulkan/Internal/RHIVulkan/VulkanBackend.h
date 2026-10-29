#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiBackend.h"
#include "RHIVulkan/VulkanApi.h"
#include "String/Name.h"
#include "String/String.h"
#include "RHIVulkan/VulkanBackend.gen.h"

namespace Marlin
{
/// A physical device that meets the hardware baseline.
struct VulkanAdapter
{
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    RhiAdapterInfo Info;
    List<VkExtensionProperties> Extensions;

    [[nodiscard]] bool HasExtension(const char *name) const;
};

/// The Vulkan 1.4 backend. Capture and crash tools reach it between Initialize and CreateDevice, through
/// IGpuCaptureTool::PrepareBackend and IGpuCrashTool::Initialize, to ask for device extensions and features.
MCLASS()
class RHIVULKAN_API VulkanBackend : public IRhiBackend
{
    MGENERATED_BODY()
public:
    VulkanBackend() = default;
    ~VulkanBackend() override;
    MARLIN_NONCOPYABLE(VulkanBackend);

    [[nodiscard]] Name GetName() const override;
    [[nodiscard]] int32 GetPriority() const override;
    bool Initialize(RhiApiValidation validation) override;
    void Shutdown() override;
    [[nodiscard]] Span<const RhiAdapterInfo> GetAdapters() const override
    {
        return _adapterInfos;
    }
    [[nodiscard]] Result<RhiDevice *, RhiError> CreateDevice(const RhiDeviceDesc &desc) override;
    void DestroyDevice(RhiDevice *device) override;

    [[nodiscard]] VkInstance GetInstance() const
    {
        return _instance;
    }

    [[nodiscard]] Span<const VulkanAdapter> GetVulkanAdapters() const
    {
        return _adapters;
    }

    /// Before Initialize: loads the API from *library* instead of the system loader, such as Streamline's
    /// interposer, which then sees every instance, device and swapchain call.
    void SetLoaderLibrary(StringView library);
    /// Before Initialize: enables an instance extension if the loader has it.
    void RequestInstanceExtension(const char *name);

    /// Enables a device extension, if every adapter that may be picked has it; false otherwise.
    bool RequestDeviceExtension(const char *name);
    /// Chains a feature or create-info structure, which must outlive CreateDevice, into VkDeviceCreateInfo.
    void ChainDeviceCreateInfo(VkBaseOutStructure *structure);
    /// Before CreateDevice: makes *count* more queues in the family of *queue* for an SDK, such as frame
    /// generation; fewer if the family has fewer.
    void RequestAdditionalQueues(RhiQueueType queue, uint32 count);

    /// Where a device queue is, for SDKs that take the queues of the device.
    struct QueueLocation
    {
        uint32 Family = ~0u;
        uint32 FirstIndex = 0;
        uint32 Count = 0;
    };

    /// After CreateDevice: the queue the device uses for *queue*, and the additional queues made for SDKs.
    [[nodiscard]] QueueLocation GetDeviceQueue(RhiQueueType queue) const
    {
        return _deviceQueues[static_cast<uint32>(queue)];
    }

    [[nodiscard]] QueueLocation GetAdditionalQueues(RhiQueueType queue) const
    {
        return _additionalQueues[static_cast<uint32>(queue)];
    }

    [[nodiscard]] uint32 GetRequestedAdditionalQueues(RhiQueueType queue) const
    {
        return _requestedQueues[static_cast<uint32>(queue)];
    }

    /// The device sets where it put its queues.
    void SetQueueLocations(const QueueLocation (&device)[RhiQueueTypeCount],
                           const QueueLocation (&additional)[RhiQueueTypeCount]);

    [[nodiscard]] bool HasDebugUtils() const
    {
        return _debugUtils;
    }

    [[nodiscard]] bool HasSwapchainColorSpaces() const
    {
        return _swapchainColorSpaces;
    }

private:
    static VkBool32 VKAPI_PTR OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT *data, void *context);
    bool CreateInstance(RhiApiValidation validation);
    void FindAdapters();

    VkInstance _instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _messenger = VK_NULL_HANDLE;
    RhiApiValidation _validation = RhiApiValidation::None;
    bool _debugUtils = false;
    bool _swapchainColorSpaces = false;
    List<VulkanAdapter> _adapters;
    List<RhiAdapterInfo> _adapterInfos;
    List<const char *> _requestedExtensions;
    List<VkBaseOutStructure *> _chained;
    String _loaderLibrary;
    List<const char *> _requestedInstanceExtensions;
    uint32 _requestedQueues[RhiQueueTypeCount] = {};
    QueueLocation _deviceQueues[RhiQueueTypeCount];
    QueueLocation _additionalQueues[RhiQueueTypeCount];
};
} // namespace Marlin
