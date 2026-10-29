#include "Testing/Testing.h"

#include "Containers/FrozenArray.h"
#include "Reflection/Reflection.h"
#include "Reflection/TypeOf.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
MARLIN_TEST(Containers, FrozenArray)
{
    const int32 values[] = {1, 2, 3};
    FrozenArray<int32> owned{Span<const int32>(values)};
    MARLIN_EXPECT(owned.IsOwned() && owned.Count() == 3 && owned[2] == 3);
    owned.Resize(5);
    MARLIN_EXPECT(owned.Count() == 5 && owned[1] == 2 && owned[4] == 0);
    owned.RemoveAt(0);
    MARLIN_EXPECT(owned.Count() == 4 && owned[0] == 2);

    // A block lends its elements: copies and writes go to the heap, the borrowed array frees nothing.
    int32 block[] = {7, 8};
    FrozenArray<int32> borrowed;
    Private::FrozenStorage storage{block, 2, 0};
    MemCopy(&borrowed, &storage, sizeof(storage));
    MARLIN_EXPECT(!borrowed.IsOwned() && borrowed[1] == 8);
    FrozenArray<int32> copy = borrowed;
    MARLIN_EXPECT(copy.IsOwned() && copy == borrowed);
    copy[0] = 9;
    MARLIN_EXPECT(block[0] == 7);
    borrowed.Resize(3);
    MARLIN_EXPECT(borrowed.IsOwned() && borrowed[0] == 7 && borrowed[2] == 0);

    FrozenArray<FrozenString> names;
    names.Resize(2);
    names[1] = FrozenString("Two");
    FrozenArray<FrozenString> moved = Move(names);
    MARLIN_EXPECT(names.IsEmpty() && moved[1].AsView() == "Two" && moved[0].IsEmpty());
    MARLIN_EXPECT(*moved[0].CStr() == '\0');
}

MARLIN_TEST(Containers, FrozenArrayReflection)
{
    const TypeInfo &array = TypeOf<FrozenArray<int32>>();
    MARLIN_EXPECT(array.GetKind() == TypeKind::List && array.GetName() == "FrozenArray<int32>");
    MARLIN_EXPECT(array.GetExtension() == &Private::FrozenArrayLayout);
    FrozenArray<int32> a;
    FrozenArray<int32> b;
    array.GetListOperations().Resize(&a, 2);
    *static_cast<int32 *>(array.GetListOperations().At(&a, 1)) = 5;
    MARLIN_EXPECT(Reflection::Copy(array, &b, &a) && b.Count() == 2 && b[1] == 5);
    MARLIN_EXPECT(Reflection::Equals(array, &a, &b));

    const TypeInfo &string = TypeOf<FrozenString>();
    MARLIN_EXPECT(string.GetKind() == TypeKind::Opaque && string.GetExtension() == &Private::FrozenStringLayout);
    FrozenString text;
    MARLIN_EXPECT(Reflection::ParseValue("Hello", string, &text) && text.AsView() == "Hello");
    StringBuilder<32> output;
    Reflection::FormatValue(output, string, &text);
    MARLIN_EXPECT(output.AsView() == "Hello");
}
} // namespace Marlin::Tests
