#include "EditorShellTestTypes.h"

namespace Marlin::Tests
{
namespace
{
class TestContext final : public EditingContext
{
public:
    bool Pinned = false;

    [[nodiscard]] bool IsPinned() const override
    {
        return Pinned;
    }

    TestContext(const EditingContextType *type, const Guid &asset, StringView name)
        : EditingContext(type), _asset(asset)
    {
        Title.Set(String(name));
    }

    [[nodiscard]] Guid GetAsset() const override
    {
        return _asset;
    }

    [[nodiscard]] UIElement BuildContent() override
    {
        return Box();
    }

private:
    Guid _asset;
};
} // namespace

UniquePtr<EditingContext> TestAssetContextType::Create(const Guid &asset, StringView name) const
{
    return MakeUnique<TestContext>(this, asset, name);
}

UniquePtr<EditingContext> TestPinnedContextType::Create() const
{
    UniquePtr<TestContext> context = MakeUnique<TestContext>(this, Guid(), GetDisplayName());
    context->Pinned = true;
    return context;
}

UniquePtr<EditingContext> TestToolContextType::Create() const
{
    return MakeUnique<TestContext>(this, Guid(), GetDisplayName());
}

bool TestDockType::CanDockIn(const EditingContext &context) const
{
    return context.GetType() == nullptr;
}
} // namespace Marlin::Tests
