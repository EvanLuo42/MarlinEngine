#include "RHIVulkan/VulkanBackend.h"

#include "Diagnostics/Log.h"
#include "HAL/PlatformDebug.h"
#include "RHI/RhiLog.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "VulkanDevice.h"
#include "VulkanFeatures.h"
#include "VulkanPlatform.h"

namespace Marlin
{
namespace
{
constexpr const char *ValidationLayer = "VK_LAYER_KHRONOS_validation";

bool HasExtension(Span<const VkExtensionProperties> extensions, const char *name)
{
    for (const VkExtensionProperties &extension : extensions)
    {
        if (StringView(extension.extensionName) == StringView(name))
        {
            return true;
        }
    }
    return false;
}

void EnumerateInstanceExtensions(const char *layer, List<VkExtensionProperties> &extensions)
{
    uint32 count = 0;
    Vk.vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
    extensions.Resize(count);
    Vk.vkEnumerateInstanceExtensionProperties(layer, &count, extensions.Data());
    extensions.Resize(count);
}

String FormatDriverVersion(const VkPhysicalDeviceProperties &properties)
{
    StringBuilder<64> text;
    const uint32 version = properties.driverVersion;
    if (properties.vendorID == 0x10DE)
    {
        FormatTo(text, "{0}.{1}", (version >> 22) & 0x3FF, (version >> 14) & 0xFF);
    }
    else
    {
        FormatTo(text, "{0}.{1}.{2}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                 VK_API_VERSION_PATCH(version));
    }
    return String(text.AsView());
}
} // namespace

bool VulkanAdapter::HasExtension(const char *name) const
{
    return Marlin::HasExtension(Extensions, name);
}

VulkanBackend::~VulkanBackend()
{
    Shutdown();
}

Name VulkanBackend::GetName() const
{
    return Name("Vulkan");
}

int32 VulkanBackend::GetPriority() const
{
    return 100;
}

bool VulkanBackend::Initialize(RhiApiValidation validation)
{
    if (!Vk.LoadGlobal(_loaderLibrary.IsEmpty() ? nullptr : _loaderLibrary.CStr()))
    {
        MARLIN_LOG(LogRhi, Info, "No Vulkan loader ({0})",
                   _loaderLibrary.IsEmpty() ? StringView(VulkanPlatform::GetLoaderName()) : _loaderLibrary.AsView());
        return false;
    }
    uint32 version = 0;
    Vk.vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_4)
    {
        MARLIN_LOG(LogRhi, Warning, "The Vulkan loader is {0}.{1}; Vulkan 1.4 is required",
                   VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
        Vk.Unload();
        return false;
    }
    if (!CreateInstance(validation))
    {
        Vk.Unload();
        return false;
    }
    FindAdapters();
    if (_adapters.IsEmpty())
    {
        MARLIN_LOG(LogRhi, Warning, "No Vulkan device meets the hardware baseline");
        Shutdown();
        return false;
    }
    return true;
}

bool VulkanBackend::CreateInstance(RhiApiValidation validation)
{
    List<VkExtensionProperties> available;
    EnumerateInstanceExtensions(nullptr, available);

    List<const char *> extensions;
    extensions.Add(VK_KHR_SURFACE_EXTENSION_NAME);
    extensions.Add(VulkanPlatform::GetSurfaceExtension());
    if (HasExtension(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
    {
        extensions.Add(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }
    _swapchainColorSpaces = HasExtension(available, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (_swapchainColorSpaces)
    {
        extensions.Add(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }
    for (const char *requested : _requestedInstanceExtensions)
    {
        if (HasExtension(available, requested) && !extensions.Contains(requested))
        {
            extensions.Add(requested);
        }
        else if (!HasExtension(available, requested))
        {
            MARLIN_LOG(LogRhi, Warning, "The Vulkan loader has no {0}", requested);
        }
    }
    _debugUtils = !MARLIN_BUILD_SHIPPING && HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (_debugUtils)
    {
        extensions.Add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    List<const char *> layers;
    const VkBool32 enabled = VK_TRUE;
    List<VkLayerSettingEXT> settings;
    VkLayerSettingsCreateInfoEXT layerSettings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
    _validation = validation;
    if (validation != RhiApiValidation::None)
    {
        uint32 count = 0;
        Vk.vkEnumerateInstanceLayerProperties(&count, nullptr);
        List<VkLayerProperties> availableLayers;
        availableLayers.Resize(count);
        Vk.vkEnumerateInstanceLayerProperties(&count, availableLayers.Data());
        const bool found = availableLayers.Find([](const VkLayerProperties &layer) {
            return StringView(layer.layerName) == StringView(ValidationLayer);
        }) != nullptr;
        if (!found)
        {
            MARLIN_LOG(LogRhi, Warning, "Vulkan validation was asked for, but {0} is not installed", ValidationLayer);
            _validation = RhiApiValidation::None;
        }
        else
        {
            layers.Add(ValidationLayer);
            List<VkExtensionProperties> layerExtensions;
            EnumerateInstanceExtensions(ValidationLayer, layerExtensions);
            if (HasExtension(layerExtensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME))
            {
                extensions.Add(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
                if (HasAnyFlags(validation, RhiApiValidation::GpuAssisted))
                {
                    settings.Add({ValidationLayer, "gpuav_enable", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enabled});
                }
                if (HasAnyFlags(validation, RhiApiValidation::Sync))
                {
                    settings.Add({ValidationLayer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enabled});
                }
                layerSettings.settingCount = static_cast<uint32>(settings.Count());
                layerSettings.pSettings = settings.Data();
            }
            if (!_debugUtils && HasExtension(layerExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            {
                _debugUtils = true;
                extensions.Add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Marlin";
    application.pEngineName = "Marlin Engine";
    application.apiVersion = VK_API_VERSION_1_4;

    VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = &OnDebugMessage;
    messenger.pUserData = this;
    messenger.pNext = layerSettings.settingCount > 0 ? &layerSettings : nullptr;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<uint32>(extensions.Count());
    info.ppEnabledExtensionNames = extensions.Data();
    info.enabledLayerCount = static_cast<uint32>(layers.Count());
    info.ppEnabledLayerNames = layers.Data();
    if (_debugUtils && _validation != RhiApiValidation::None)
    {
        info.pNext = &messenger;
    }
    else if (layerSettings.settingCount > 0)
    {
        info.pNext = &layerSettings;
    }
    const VkResult result = Vk.vkCreateInstance(&info, nullptr, &_instance);
    if (result != VK_SUCCESS)
    {
        MARLIN_LOG(LogRhi, Warning, "vkCreateInstance failed ({0})", static_cast<int32>(result));
        return false;
    }
    Vk.LoadInstance(_instance);
    if (_debugUtils && _validation != RhiApiValidation::None)
    {
        messenger.pNext = nullptr;
        Vk.vkCreateDebugUtilsMessengerEXT(_instance, &messenger, nullptr, &_messenger);
    }
    MARLIN_LOG(LogRhi, Info, "Vulkan instance created; validation: {0}{1}{2}",
               HasAnyFlags(_validation, RhiApiValidation::Api) ? "Api " : "None ",
               HasAnyFlags(_validation, RhiApiValidation::GpuAssisted) ? "GpuAssisted " : "",
               HasAnyFlags(_validation, RhiApiValidation::Sync) ? "Sync" : "");
    return true;
}

void VulkanBackend::FindAdapters()
{
    uint32 count = 0;
    Vk.vkEnumeratePhysicalDevices(_instance, &count, nullptr);
    List<VkPhysicalDevice> devices;
    devices.Resize(count);
    Vk.vkEnumeratePhysicalDevices(_instance, &count, devices.Data());
    for (VkPhysicalDevice physicalDevice : devices)
    {
        VulkanAdapter adapter;
        adapter.PhysicalDevice = physicalDevice;
        uint32 extensionCount = 0;
        Vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        adapter.Extensions.Resize(extensionCount);
        Vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, adapter.Extensions.Data());

        VulkanPropertyChain properties;
        properties.Query(adapter);
        const VkPhysicalDeviceProperties &core = properties.Properties.properties;
        StringBuilder<512> missing;
        if (core.apiVersion < VK_API_VERSION_1_4)
        {
            missing.Append("Vulkan 1.4");
        }
        for (const char *extension : VulkanRequiredExtensions)
        {
            if (!adapter.HasExtension(extension))
            {
                FormatTo(missing, "{0}{1}", missing.AsView().IsEmpty() ? "" : ", ", extension);
            }
        }
        if (missing.AsView().IsEmpty())
        {
            VulkanFeatureChain features;
            features.Query(adapter);
            features.CheckBaseline(missing);
        }
        if (!missing.AsView().IsEmpty())
        {
            MARLIN_LOG(LogRhi, Info, "{0} does not meet the baseline: no {1}", core.deviceName, missing.AsView());
            continue;
        }

        RhiAdapterInfo &info = adapter.Info;
        info.Name = String(core.deviceName);
        info.VendorId = core.vendorID;
        info.DeviceId = core.deviceID;
        info.DriverVersion = FormatDriverVersion(core);
        switch (core.deviceType)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            info.Kind = RhiAdapterKind::Discrete;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            info.Kind = RhiAdapterKind::Integrated;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            info.Kind = RhiAdapterKind::Software;
            break;
        default:
            info.Kind = RhiAdapterKind::Other;
            break;
        }
        VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        Vk.vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &memory);
        const VkPhysicalDeviceMemoryProperties &heaps = memory.memoryProperties;
        bool hostVisibleOnly = true;
        for (uint32 i = 0; i < heaps.memoryHeapCount; ++i)
        {
            if ((heaps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            {
                info.DeviceMemoryBytes += heaps.memoryHeaps[i].size;
            }
        }
        for (uint32 i = 0; i < heaps.memoryTypeCount; ++i)
        {
            const VkMemoryType &type = heaps.memoryTypes[i];
            constexpr VkMemoryPropertyFlags hostDevice =
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            if ((type.propertyFlags & hostDevice) == hostDevice)
            {
                const uint64 size = heaps.memoryHeaps[type.heapIndex].size;
                info.HostVisibleDeviceMemoryBytes =
                    size > info.HostVisibleDeviceMemoryBytes ? size : info.HostVisibleDeviceMemoryBytes;
            }
            if ((type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            {
                hostVisibleOnly = false;
            }
        }
        info.UnifiedMemory = info.Kind == RhiAdapterKind::Integrated || hostVisibleOnly;
        _adapters.Add(Move(adapter));
    }
    _adapters.StableSort([](const VulkanAdapter &a, const VulkanAdapter &b) {
        if (a.Info.Kind != b.Info.Kind)
        {
            return a.Info.Kind == RhiAdapterKind::Discrete;
        }
        return a.Info.DeviceMemoryBytes > b.Info.DeviceMemoryBytes;
    });
    for (const VulkanAdapter &adapter : _adapters)
    {
        _adapterInfos.Add(adapter.Info);
    }
}

void VulkanBackend::Shutdown()
{
    if (_instance != VK_NULL_HANDLE)
    {
        if (_messenger != VK_NULL_HANDLE)
        {
            Vk.vkDestroyDebugUtilsMessengerEXT(_instance, _messenger, nullptr);
            _messenger = VK_NULL_HANDLE;
        }
        Vk.vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
        Vk.Unload();
    }
    _adapters.Clear();
    _adapterInfos.Clear();
}

bool VulkanBackend::RequestDeviceExtension(const char *name)
{
    for (const VulkanAdapter &adapter : _adapters)
    {
        if (!adapter.HasExtension(name))
        {
            MARLIN_LOG(LogRhi, Warning, "{0} has no {1}", adapter.Info.Name, name);
            return false;
        }
    }
    _requestedExtensions.Add(name);
    return true;
}

void VulkanBackend::ChainDeviceCreateInfo(VkBaseOutStructure *structure)
{
    _chained.Add(structure);
}

void VulkanBackend::SetLoaderLibrary(StringView library)
{
    MARLIN_ASSERT(_instance == VK_NULL_HANDLE);
    _loaderLibrary = String(library);
}

void VulkanBackend::RequestInstanceExtension(const char *name)
{
    MARLIN_ASSERT(_instance == VK_NULL_HANDLE);
    _requestedInstanceExtensions.Add(name);
}

void VulkanBackend::RequestAdditionalQueues(RhiQueueType queue, uint32 count)
{
    _requestedQueues[static_cast<uint32>(queue)] += count;
}

void VulkanBackend::SetQueueLocations(const QueueLocation (&device)[RhiQueueTypeCount],
                                      const QueueLocation (&additional)[RhiQueueTypeCount])
{
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        _deviceQueues[queue] = device[queue];
        _additionalQueues[queue] = additional[queue];
    }
}

Result<RhiDevice *, RhiError> VulkanBackend::CreateDevice(const RhiDeviceDesc &desc)
{
    const int32 index = desc.AdapterIndex >= 0 && desc.AdapterIndex < static_cast<int32>(_adapters.Count())
                            ? desc.AdapterIndex
                            : 0;
    auto *device = new VulkanDevice(*this, _adapters[static_cast<usize>(index)], desc);
    const Result<void, RhiError> result = device->Initialize(_requestedExtensions, _chained);
    if (result.IsError())
    {
        delete device;
        return Failure(result.Error());
    }
    return static_cast<RhiDevice *>(device);
}

void VulkanBackend::DestroyDevice(RhiDevice *device)
{
    delete static_cast<VulkanDevice *>(device);
}

VkBool32 VulkanBackend::OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                       VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                       const VkDebugUtilsMessengerCallbackDataEXT *data, void * /*context*/)
{
    StringBuilder<4096> message;
    message.Append(data->pMessage != nullptr ? data->pMessage : "");
    for (uint32 i = 0; i < data->objectCount; ++i)
    {
        if (data->pObjects[i].pObjectName != nullptr)
        {
            FormatTo(message, "\n  object: {0}", data->pObjects[i].pObjectName);
        }
    }
    for (uint32 i = 0; i < data->cmdBufLabelCount; ++i)
    {
        FormatTo(message, "\n  in marker: {0}", data->pCmdBufLabels[i].pLabelName);
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
        MARLIN_LOG(LogRhi, Error, "{0}", message.AsView());
        if (PlatformDebug::IsDebuggerAttached())
        {
            __builtin_debugtrap();
        }
    }
    else
    {
        MARLIN_LOG(LogRhi, Warning, "{0}", message.AsView());
    }
    return VK_FALSE;
}
} // namespace Marlin
