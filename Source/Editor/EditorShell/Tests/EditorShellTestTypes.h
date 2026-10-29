#pragma once

#include "CoreTypes.h"
#include "EditorShell/Dock.h"
#include "EditorShell/EditingContext.h"
#include "EditorShellTestTypes.gen.h"

namespace Marlin::Tests
{
/// Edits any asset no other type takes, showing nothing.
MCLASS(Priority = -1000)
class TestAssetContextType final : public AssetContextType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetIcon() const override
    {
        return "Square";
    }
    [[nodiscard]] StringView GetIconColor() const override
    {
        return "Tab.Icon.Asset";
    }
    [[nodiscard]] bool CanEdit(StringView /*assetType*/) const override
    {
        return true;
    }
    [[nodiscard]] UniquePtr<EditingContext> Create(const Guid &asset, StringView name) const override;
};

MCLASS()
class TestToolContextType final : public ToolContextType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetIcon() const override
    {
        return "Settings";
    }
    [[nodiscard]] StringView GetIconColor() const override
    {
        return "Tab.Icon.Tool";
    }
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Test Tool";
    }
    [[nodiscard]] UniquePtr<EditingContext> Create() const override;
};

/// Always open, as the Asset Manager is.
MCLASS()
class TestPinnedContextType final : public PinnedContextType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetIcon() const override
    {
        return "FolderOpen";
    }
    [[nodiscard]] StringView GetIconColor() const override
    {
        return "Tab.Icon.AssetManager";
    }
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Test Pinned";
    }
    [[nodiscard]] UniquePtr<EditingContext> Create() const override;
};

/// Docks of the test contexts, which hold any dock that is not another context's.
MCLASS(Abstract)
class TestDockType : public DockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] bool CanDockIn(const EditingContext &context) const override;
};

MCLASS()
class TestAlphaDock final : public TestDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Alpha";
    }
};

MCLASS()
class TestBetaDock final : public TestDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Beta";
    }
};

MCLASS()
class TestGammaDock final : public TestDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Gamma";
    }
};

/// A context that edits nothing, made directly by tests.
class TestEditingContext final : public EditingContext
{
public:
    TestEditingContext() : EditingContext(nullptr)
    {
    }

    [[nodiscard]] UIElement BuildContent() override
    {
        return Box();
    }
};
} // namespace Marlin::Tests
