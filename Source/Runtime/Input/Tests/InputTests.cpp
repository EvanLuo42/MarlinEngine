#include "Testing/Testing.h"

#include "Asset/AssetBlock.h"
#include "Asset/Assets.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformFile.h"
#include "Input/InputActions.h"
#include "Input/InputSubsystem.h"
#include "InputTestTypes.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
/// Serves the blocks written under /Temp/InputTests.
class InputBlockStore final : public IAssetStore
{
public:
    explicit InputBlockStore(HashMap<Guid, String> files) : _files(Move(files))
    {
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const String *path = _files.Find(guid);
        AssetLocation location;
        done(path != nullptr && Vfs::OpenRegion(path->AsView(), location.Block) ? &location : nullptr);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "Input test blocks";
    }

private:
    HashMap<Guid, String> _files;
};

Guid MakeGuid(uint32 value)
{
    return {0x1A9B7E57u, 0, 0, value};
}

/// One binding of a test context.
struct BindingDesc
{
    uint32 Action = 0;
    StringView Control;
    StringView Chord;
    InputAxis XTo = InputAxis::X;
    InputAxis YTo = InputAxis::Y;
    Vector3 Scale = Vector3::One;
    StringView Name;

    BindingDesc WithChord(StringView chord) const
    {
        BindingDesc copy = *this;
        copy.Chord = chord;
        return copy;
    }
    BindingDesc WithXTo(InputAxis axis) const
    {
        BindingDesc copy = *this;
        copy.XTo = axis;
        return copy;
    }
    BindingDesc WithYTo(InputAxis axis) const
    {
        BindingDesc copy = *this;
        copy.YTo = axis;
        return copy;
    }
    BindingDesc WithScale(Vector3 scale) const
    {
        BindingDesc copy = *this;
        copy.Scale = scale;
        return copy;
    }
    BindingDesc WithName(StringView name) const
    {
        BindingDesc copy = *this;
        copy.Name = name;
        return copy;
    }
};

BindingDesc Bind(uint32 action, StringView control)
{
    BindingDesc desc;
    desc.Action = action;
    desc.Control = control;
    return desc;
}

/// The asset table with the tests' actions and contexts converted into blocks, and an InputSubsystem whose
/// controls the tests set. Actions and contexts are numbered; their Guids come from MakeGuid.
class InputFixture
{
public:
    InputFixture()
    {
        Assets::Initialize();
        StringBuilder<512> physical;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/InputTests", physical));
        _directory = physical.AsView();
        (void)PlatformFile::DeleteDirectory(_directory.CStr(), true);
        MARLIN_REQUIRE(PlatformFile::CreateDirectory(_directory.CStr()));
    }

    ~InputFixture()
    {
        _input.Reset();
        _contexts.Clear();
        Assets::RemoveStore(_store);
        Assets::Shutdown();
        (void)PlatformFile::DeleteDirectory(_directory.CStr(), true);
        InputSubsystem::Rebinds.Clear();
    }

    MARLIN_NONCOPYABLE(InputFixture);

    void AddAction(uint32 id, InputValueType type, bool accumulate = false)
    {
        InputAction action;
        action.ValueType = type;
        action.Accumulate = accumulate;
        Write(action, MakeGuid(id));
    }

    void AddContext(uint32 id, int32 priority, Span<const BindingDesc> bindings)
    {
        InputContext context;
        context.Name = FrozenString(StringView("TestContext"));
        context.Priority = priority;
        context.Bindings.Resize(bindings.Count());
        for (usize i = 0; i < bindings.Count(); ++i)
        {
            const BindingDesc &desc = bindings[i];
            InputBinding &binding = context.Bindings[i];
            binding.Action = AssetRef<InputAction>::FromGuid(MakeGuid(desc.Action));
            binding.Control = FrozenString(desc.Control);
            if (!desc.Chord.IsEmpty())
            {
                binding.Chord.Resize(1);
                binding.Chord[0] = FrozenString(desc.Chord);
            }
            binding.XTo = desc.XTo;
            binding.YTo = desc.YTo;
            binding.Scale = desc.Scale;
            binding.Name = FrozenString(desc.Name);
        }
        Write(context, MakeGuid(id));
    }

    /// Serves the blocks, loads the contexts and starts the subsystem; after the Adds.
    void Start()
    {
        _store = Assets::AddStore(MakeUnique<InputBlockStore>(_files), 0);
        _input = MakeUnique<InputSubsystem>();
        SubsystemContext context;
        _input->Initialize(context);
    }

    AssetRef<InputContext> Context(uint32 id)
    {
        AssetRef<InputContext> context = Assets::LoadSync<InputContext>(MakeGuid(id));
        MARLIN_REQUIRE(context.IsLoaded());
        _contexts.Add(context);
        return context;
    }

    InputSubsystem &Input()
    {
        return *_input;
    }

    void Set(StringView control, Vector2 value)
    {
        InputControls &controls = _input->GetControlsForTesting();
        const InputControlId id = controls.Find(control);
        MARLIN_REQUIRE(id != InvalidInputControl);
        if (controls.IsAmount(id))
        {
            controls.Add(id, value);
        }
        else
        {
            controls.Set(id, value);
        }
    }

    /// One frame: the queued requests and the controls as set, then the amounts start again from zero.
    void Frame(float64 delta = 1.0 / 60)
    {
        _input->ApplyRequests();
        _input->Evaluate(delta);
        _input->GetControlsForTesting().BeginFrame();
    }

private:
    void Write(const Asset &asset, const Guid &guid)
    {
        List<uint8> block;
        MARLIN_REQUIRE(AssetBlock::Write(asset, guid, block));
        StringBuilder<64> name;
        name.Append("/Temp/InputTests/");
        guid.FormatTo(name, "N");
        name.Append(".mablk");
        MARLIN_REQUIRE(
            Vfs::WriteAll(name.AsView(), StringView(reinterpret_cast<const char *>(block.Data()), block.Count())));
        _files.FindOrAdd(guid) = name.AsView();
    }

    String _directory;
    HashMap<Guid, String> _files;
    AssetStoreId _store = 0;
    UniquePtr<InputSubsystem> _input;
    List<AssetRef<InputContext>> _contexts;
};

/// What a stage sees: the world resource and the stage's memory of the records.
struct Stage
{
    InputActions Actions;
    HashMap<Guid, InputSeen> Seen;

    const InputActionValue &Sample(const InputSubsystem &input, uint32 action)
    {
        Actions.Sample(input.GetRecords(), Seen);
        return Actions.Get(MakeGuid(action));
    }

    const InputActionValue &Get(uint32 action) const
    {
        return Actions.Get(MakeGuid(action));
    }
};

bool Near(float32 a, float32 b)
{
    return Math::Abs(a - b) < 1e-4f;
}
} // namespace

MARLIN_TEST(Input, BindingsMapControlsOntoActions)
{
    InputFixture fixture;
    fixture.AddAction(1, InputValueType::Axis3D);
    fixture.AddAction(2, InputValueType::Button);
    const BindingDesc bindings[] = {
        Bind(1, "Test.A").WithXTo(InputAxis::Z).WithScale(Vector3(1, 1, -1)),
        Bind(1, "Test.Stick").WithYTo(InputAxis::Z).WithScale(Vector3(1, 1, -1)),
        Bind(2, "Test.B").WithChord("Test.A"),
    };
    fixture.AddContext(10, 0, bindings);
    fixture.Start();
    fixture.Input().PushContext(fixture.Context(10));

    // The push takes effect at the next frame.
    fixture.Set("Test.A", {1, 0});
    fixture.Set("Test.Stick", {0.5f, 0.25f});
    fixture.Frame();
    Stage stage;
    const InputActionValue &move = stage.Sample(fixture.Input(), 1);
    MARLIN_EXPECTF(Near(move.Value.X, 0.5f) && Near(move.Value.Y, 0) && Near(move.Value.Z, -1.25f), "{0} {1} {2}",
                   move.Value.X, move.Value.Y, move.Value.Z);
    MARLIN_EXPECT(move.Down && move.Pressed);

    // The chord: B alone gives nothing, B with A held presses the button.
    MARLIN_EXPECT(!stage.Get(2).Down);
    fixture.Set("Test.A", {0, 0});
    fixture.Set("Test.B", {1, 0});
    fixture.Frame();
    MARLIN_EXPECT(!stage.Sample(fixture.Input(), 2).Down);
    fixture.Set("Test.A", {1, 0});
    fixture.Frame();
    const InputActionValue &button = stage.Sample(fixture.Input(), 2);
    MARLIN_EXPECT(button.Down && button.Pressed && Near(button.Value.X, 1));

    // An action no active context binds is at rest.
    MARLIN_EXPECT(!stage.Get(99).Down && stage.Get(99).Value == Vector3::Zero);
}

MARLIN_TEST(Input, HigherContextsConsumeTheirControls)
{
    InputFixture fixture;
    fixture.AddAction(1, InputValueType::Button);
    fixture.AddAction(2, InputValueType::Button);
    const BindingDesc gameplay[] = {Bind(1, "Test.A"), Bind(1, "Test.B")};
    const BindingDesc menu[] = {Bind(2, "Test.A")};
    fixture.AddContext(10, 0, gameplay);
    fixture.AddContext(11, 5, menu);
    fixture.Start();
    fixture.Input().PushContext(fixture.Context(10));
    fixture.Input().PushContext(fixture.Context(11));

    fixture.Set("Test.A", {1, 0});
    fixture.Frame();
    Stage stage;
    MARLIN_EXPECT(stage.Sample(fixture.Input(), 2).Down);
    MARLIN_EXPECT(!stage.Get(1).Down);

    // B is not the menu's, so it still reaches gameplay.
    fixture.Set("Test.B", {1, 0});
    fixture.Frame();
    MARLIN_EXPECT(stage.Sample(fixture.Input(), 1).Down);

    // Pushed twice, popped once: still active.
    fixture.Input().PushContext(fixture.Context(11));
    fixture.Input().PopContext(fixture.Context(11));
    fixture.Set("Test.B", {0, 0});
    fixture.Frame();
    MARLIN_EXPECT(stage.Sample(fixture.Input(), 2).Down && !stage.Get(1).Down);
    fixture.Input().PopContext(fixture.Context(11));
    fixture.Frame();
    MARLIN_EXPECT(!stage.Sample(fixture.Input(), 2).Down && stage.Get(1).Down);
}

MARLIN_TEST(Input, EachStageSeesWhatHappenedSinceItLastRan)
{
    InputFixture fixture;
    fixture.AddAction(1, InputValueType::Button);
    fixture.AddAction(2, InputValueType::Axis2D, true);
    const BindingDesc bindings[] = {
        Bind(1, "Test.A"),
        Bind(2, "Test.Delta").WithScale(Vector3(2, 2, 1)),
        Bind(2, "Test.Stick"),
    };
    fixture.AddContext(10, 0, bindings);
    fixture.Start();
    fixture.Input().PushContext(fixture.Context(10));
    fixture.Frame();

    Stage frame;
    Stage fixed;
    (void)frame.Sample(fixture.Input(), 1);
    (void)fixed.Sample(fixture.Input(), 1);

    // A frame that runs no fixed step: the press waits for the next one, which sees it exactly once.
    fixture.Set("Test.A", {1, 0});
    fixture.Set("Test.Delta", {3, 0});
    fixture.Frame();
    MARLIN_EXPECT(frame.Sample(fixture.Input(), 1).Pressed);
    MARLIN_EXPECT(Near(frame.Get(2).Value.X, 6));

    fixture.Set("Test.Delta", {1, 0});
    fixture.Frame();
    MARLIN_EXPECT(!frame.Sample(fixture.Input(), 1).Pressed && frame.Get(1).Down);
    MARLIN_EXPECT(Near(frame.Get(2).Value.X, 2));

    // Two fixed steps in this frame: the first sees the press and both frames' motion, the second neither.
    MARLIN_EXPECT(fixed.Sample(fixture.Input(), 1).Pressed);
    MARLIN_EXPECTF(Near(fixed.Get(2).Value.X, 8), "{0}", fixed.Get(2).Value.X);
    MARLIN_EXPECT(!fixed.Sample(fixture.Input(), 1).Pressed && fixed.Get(1).Down);
    MARLIN_EXPECT(fixed.Get(2).Value == Vector3::Zero);

    // A held stick adds its value per second to an amount.
    fixture.Set("Test.A", {0, 0});
    fixture.Set("Test.Stick", {1, -0.5f});
    fixture.Frame(0.5);
    MARLIN_EXPECT(frame.Sample(fixture.Input(), 1).Released);
    MARLIN_EXPECTF(Near(frame.Get(2).Value.X, 0.5f) && Near(frame.Get(2).Value.Y, -0.25f), "{0} {1}",
                   frame.Get(2).Value.X, frame.Get(2).Value.Y);

    // A stage that starts late sees only the latest frame, not the whole history.
    Stage late;
    MARLIN_EXPECT(late.Sample(fixture.Input(), 1).Released && !late.Get(1).Pressed);
    MARLIN_EXPECT(Near(late.Get(2).Value.X, 0.5f));
}

MARLIN_TEST(Input, RebindsReplaceNamedBindings)
{
    InputFixture fixture;
    fixture.AddAction(1, InputValueType::Button);
    const BindingDesc bindings[] = {Bind(1, "Test.A").WithName("Jump")};
    fixture.AddContext(10, 0, bindings);
    fixture.Start();
    const AssetRef<InputContext> context = fixture.Context(10);
    fixture.Input().PushContext(context);

    fixture.Set("Test.B", {1, 0});
    fixture.Frame();
    Stage stage;
    MARLIN_EXPECT(!stage.Sample(fixture.Input(), 1).Down);

    fixture.Input().SetRebind(context, "Jump", "Test.B");
    fixture.Frame();
    MARLIN_EXPECT(stage.Sample(fixture.Input(), 1).Down);

    fixture.Input().ClearRebind(context, "Jump");
    fixture.Frame();
    MARLIN_EXPECT(!stage.Sample(fixture.Input(), 1).Down);
}
} // namespace Marlin::Tests
