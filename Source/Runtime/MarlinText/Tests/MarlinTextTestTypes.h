#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Color.h"
#include "Math/Transform.h"
#include "Memory/UniquePtr.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "String/Name.h"
#include "String/String.h"
#include "Templates/Optional.h"
#include "MarlinTextTestTypes.gen.h"

namespace Marlin::Tests
{
MENUM()
enum class TextShape : uint8
{
    Round,
    Square,
};

MSTRUCT(Tuple)
struct TextPoint
{
    MGENERATED_BODY()

    MPROPERTY()
    float32 X = 0;
    MPROPERTY()
    float32 Y[2] = {};
};

MCLASS(Version = 2)
class TextNode
{
    MGENERATED_BODY()
public:
    virtual ~TextNode() = default;

    MPROPERTY()
    Optional<int32> Maybe;
    MPROPERTY()
    UniquePtr<TextNode> Child;
    MPROPERTY()
    HashMap<Name, float32> Weights;
    MPROPERTY()
    TextShape Shape = TextShape::Square;
    MPROPERTY()
    List<TextPoint> Points;
    MPROPERTY()
    String Label;
};

MCLASS()
class TextLeaf final : public TextNode
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    int32 Depth = 0;
};

MSTRUCT()
struct TextPlacement
{
    MGENERATED_BODY()

    MPROPERTY()
    Transform Where;
    MPROPERTY()
    Guid Id;
    MPROPERTY()
    DateTime When;
    MPROPERTY()
    Color32 Tint;
    MPROPERTY(Transient)
    int32 Cache = 0;
};
} // namespace Marlin::Tests
