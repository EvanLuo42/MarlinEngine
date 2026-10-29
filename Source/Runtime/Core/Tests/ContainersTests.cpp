#include "Containers/Array.h"
#include "Containers/BitArray.h"
#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "Containers/RingBuffer.h"
#include "Containers/SortedMap.h"
#include "Containers/Span.h"
#include "Containers/SparseSet.h"
#include "Memory/Arena.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Templates/Algorithm.h"
#include "Templates/Function.h"
#include "Templates/Optional.h"
#include "Templates/Result.h"
#include "Templates/Variant.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
struct Counted
{
    static inline int32 Alive = 0;
    int32 Value = 0;

    Counted()
    {
        ++Alive;
    }
    explicit Counted(int32 value) : Value(value)
    {
        ++Alive;
    }
    Counted(const Counted &other) : Value(other.Value)
    {
        ++Alive;
    }
    Counted(Counted &&other) noexcept : Value(other.Value)
    {
        ++Alive;
    }
    Counted &operator=(const Counted &) = default;
    Counted &operator=(Counted &&) noexcept = default;
    ~Counted()
    {
        --Alive;
    }
    bool operator==(const Counted &other) const
    {
        return Value == other.Value;
    }
};

uint32 NextRandom(uint32 &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

struct Node : RefCounted
{
    static inline int32 Alive = 0;
    Node()
    {
        ++Alive;
    }
    ~Node() override
    {
        --Alive;
    }
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    Node(Node &&) = delete;
    Node &operator=(Node &&) = delete;
};

enum class ParseError : uint8
{
    Empty,
    Negative,
};

Result<int32, ParseError> ParseDoubled(int32 value)
{
    if (value == 0)
    {
        return Failure(ParseError::Empty);
    }
    if (value < 0)
    {
        return Failure(ParseError::Negative);
    }
    return value * 2;
}

constexpr int32 SumConstexpr()
{
    Array<int32, 4> values = {4, 1, 3, 2};
    Sort(values.Data(), Array<int32, 4>::Count());
    Optional<int32> optional = values[0];
    return optional.Value() + values[3];
}
static_assert(SumConstexpr() == 5);
} // namespace

MARLIN_TEST(Containers, List)
{
    {
        List<Counted> list;
        for (int32 i = 0; i < 1000; ++i)
        {
            list.Emplace(i);
        }
        MARLIN_EXPECT(list.Count() == 1000 && list[999].Value == 999);
        list.Insert(0, Counted(-1));
        list.RemoveAt(500, 10);
        MARLIN_EXPECT(list.First().Value == -1 && list.Count() == 991 && list[500].Value == 509);
        MARLIN_EXPECT(list.RemoveAll([](const Counted &c) { return c.Value % 2 == 0; }) == 495);
        MARLIN_EXPECT(list.Remove(Counted(1)) && !list.Contains(Counted(1)));
        List<Counted> copy = list;
        List<Counted> moved = Move(copy);
        MARLIN_EXPECT(copy.IsEmpty() && moved == list);
        list.Resize(10);
        list.ShrinkToFit();
        MARLIN_EXPECT(list.Capacity() == 10 && list.Pop().Value == 19);
    }
    {
        // Inserting at the front grows geometrically for elements that move one by one, too.
        List<Counted> front;
        usize growths = 0;
        for (int32 i = 0; i < 4096; ++i)
        {
            const usize capacity = front.Capacity();
            front.Insert(0, Counted(i));
            growths += front.Capacity() != capacity ? 1 : 0;
        }
        MARLIN_EXPECT(growths <= 12 && front.First().Value == 4095 && front.Last().Value == 0);
    }
}

MARLIN_TEST(Containers, InlineList)
{
    MARLIN_EXPECT(Counted::Alive == 0);
    {
        InlineList<Counted, 4> small;
        for (int32 i = 0; i < 3; ++i)
        {
            small.Emplace(i);
        }
        InlineList<Counted, 4> smallMoved = Move(small);
        for (int32 i = 3; i < 20; ++i)
        {
            smallMoved.Emplace(i);
        }
        MARLIN_EXPECT(smallMoved.Count() == 20 && smallMoved[19].Value == 19);
        smallMoved.Resize(2);
        smallMoved.ShrinkToFit();
        MARLIN_EXPECT(smallMoved.Capacity() == 4 && smallMoved[1].Value == 1);
        Arena arena;
        List<int32, ArenaAllocator> arenaList(ArenaAllocator{&arena});
        for (int32 i = 0; i < 100; ++i)
        {
            arenaList.Add(i);
        }
        MARLIN_EXPECT(arenaList[99] == 99);
    }
}

MARLIN_TEST(Containers, HashMapAndSet)
{
    MARLIN_EXPECT(Counted::Alive == 0);

    {
        HashMap<int32, Counted> map;
        HashMap<int32, int32> reference;
        List<int32> keys;
        uint32 state = 1;
        bool consistent = true;
        for (int32 step = 0; step < 200000; ++step)
        {
            const auto key = static_cast<int32>(NextRandom(state) % 5000);
            switch (NextRandom(state) % 3)
            {
            case 0:
                map.Set(key, Counted(step));
                reference.Set(key, step);
                break;
            case 1:
                consistent = consistent && map.Remove(key) == reference.Remove(key);
                break;
            default: {
                const Counted *found = map.Find(key);
                const int32 *expected = reference.Find(key);
                consistent = consistent && (found == nullptr) == (expected == nullptr) &&
                             (found == nullptr || found->Value == *expected);
                break;
            }
            }
        }
        MARLIN_EXPECT(consistent && map.Count() == reference.Count());
        usize iterated = 0;
        for (const auto &[key, value] : map)
        {
            iterated += reference.Find(key) != nullptr && reference.Find(key)[0] == value.Value ? 1 : 0;
        }
        MARLIN_EXPECT(iterated == map.Count());
        HashMap<int32, Counted> mapCopy = map;
        MARLIN_EXPECT(mapCopy.Count() == map.Count());
        map.Clear();
        MARLIN_EXPECT(map.IsEmpty() && !map.Contains(1));
        map[7].Value = 70;
        MARLIN_EXPECT(map.FindOrAdd(7).Value == 70 && !map.TryAdd(7, Counted(1)));
        HashSet<uint64> set = {1, 2, 3};
        MARLIN_EXPECT(set.Contains(uint64{2}) && !set.Add(2) && set.Remove(uint64{2}) && set.Count() == 2);
        for (uint64 i = 0; i < 100000; ++i)
        {
            set.Add(i * 7919);
        }
        MARLIN_EXPECT(set.Count() == 100000 + 2 && set.Contains(uint64{7919} * 5));
        MARLIN_EXPECT(set.RemoveIf([](uint64 v) { return v % 2 == 0; }) > 40000);

        HashMap<StringView, int32> named;
        const StringView text = "Hashed";
        named.AddHashed(text, text.GetHash(), 5);
        const int32 *found = named.FindHashed(text, text.GetHash());
        MARLIN_EXPECT(found != nullptr && *found == 5 && *named.Find(StringView("Hashed")) == 5);
    }
}

MARLIN_TEST(Containers, SortedBitsRingSparse)
{
    MARLIN_EXPECT(Counted::Alive == 0);

    {
        SortedMap<int32, int32> sorted;
        for (int32 i = 0; i < 100; ++i)
        {
            sorted.Set((i * 37) % 100, i);
        }
        int32 previous = -1;
        bool ordered = true;
        for (const auto &[key, value] : sorted)
        {
            ordered = ordered && key > previous;
            previous = key;
        }
        MARLIN_EXPECT(ordered && sorted.Count() == 100 && *sorted.Find(37) == 1 && sorted.Remove(37) &&
                      !sorted.Contains(37));

        BitArray<> bits(130);
        bits.Set(3);
        bits.Set(129);
        MARLIN_EXPECT(bits.CountSet() == 2 && bits.Find(true) == 3 && bits.Find(true, 4) == 129 &&
                      bits.Find(false) == 0);
        bits.Resize(200, true);
        MARLIN_EXPECT(bits.CountSet() == 72 && bits[150]);

        RingBuffer<Counted> ring;
        for (int32 i = 0; i < 100; ++i)
        {
            ring.PushBack(i);
            ring.PushFront(-i);
        }
        MARLIN_EXPECT(ring.Count() == 200 && ring.Front().Value == -99 && ring.Back().Value == 99);
        MARLIN_EXPECT(ring.PopFront().Value == -99 && ring.PopBack().Value == 99 && ring[0].Value == -98);

        SparseSet<> sparse;
        sparse.Add(5);
        sparse.Add(100000);
        sparse.Add(42);
        MARLIN_EXPECT(sparse.Contains(100000) && sparse.IndexOf(42) == 2 && sparse.Remove(5) == 0 &&
                      sparse.IndexOf(42) == 0);
    }
}

MARLIN_TEST(Containers, OptionalResultVariantFunction)
{
    MARLIN_EXPECT(Counted::Alive == 0);

    {
        Optional<Counted> optional;
        MARLIN_EXPECT(!optional && optional.ValueOr(Counted(3)).Value == 3);
        optional.Emplace(5);
        Optional<Counted> other = optional;
        MARLIN_EXPECT(other.HasValue() && other->Value == 5 && other == Counted(5));
        other = NullOptional;
        MARLIN_EXPECT(!other.HasValue());

        MARLIN_EXPECT(ParseDoubled(4).Value() == 8 && ParseDoubled(0).Error() == ParseError::Empty &&
                      ParseDoubled(-1).IsError());

        Variant<int32, float32, Counted> variant = Counted(9);
        MARLIN_EXPECT(variant.Is<Counted>() && variant.Get<Counted>().Value == 9);
        variant = 2.5f;
        MARLIN_EXPECT(variant.Is<float32>() && variant.TryGet<int32>() == nullptr);
        const float32 doubled =
            variant.Visit([](const auto &value) { return static_cast<float32>(value == value) * 2; });
        MARLIN_EXPECT(doubled == 2.0f);
        Variant<int32, float32, Counted> variantCopy = variant;
        MARLIN_EXPECT(variantCopy == variant);

        int32 captured = 10;
        Function<int32(int32)> small = [captured](int32 x) { return x + captured; };
        int64 big[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        Function<int64()> large = [big] { return big[0] + big[7]; };
        Function<int32(int32)> movedFunction = Move(small);
        MARLIN_EXPECT(!small && movedFunction(5) == 15 && large() == 9);
        auto lambda = [](int32 x) { return x * 3; };
        FunctionRef<int32(int32)> reference = lambda;
        MARLIN_EXPECT(reference(4) == 12);
    }
}

MARLIN_TEST(Containers, SmartPointers)
{
    MARLIN_EXPECT(Counted::Alive == 0);

    {
        UniquePtr<Counted> unique = MakeUnique<Counted>(4);
        UniquePtr<Counted> uniqueMoved = Move(unique);
        MARLIN_EXPECT(!unique && uniqueMoved->Value == 4);
        SharedPtr<Counted> shared = MakeShared<Counted>(6);
        WeakPtr<Counted> weak = shared;
        {
            SharedPtr<Counted> another = shared;
            MARLIN_EXPECT(shared.GetReferenceCount() == 2 && weak.Lock()->Value == 6);
        }
        shared.Reset();
        MARLIN_EXPECT(weak.IsExpired() && !weak.Lock());
        SharedPtr<Node> node = MakeShared<Node>();
        SharedPtr<Node> fromRaw(node.Get());
        MARLIN_EXPECT(node.GetReferenceCount() == 2 && Node::Alive == 1);
        node.Reset();
        fromRaw.Reset();
        MARLIN_EXPECT(Node::Alive == 0);
        SharedPtr<Counted> separate(new Counted(1));
        List<SharedPtr<Counted>> owners;
        for (int32 i = 0; i < 100; ++i)
        {
            owners.Add(separate);
        }
        MARLIN_EXPECT(separate.GetReferenceCount() == 101);
    }
}

MARLIN_TEST(Containers, Sorting)
{
    MARLIN_EXPECT(Counted::Alive == 0);

    {
        List<int32> values;
        uint32 state = 7;
        for (int32 i = 0; i < 100000; ++i)
        {
            values.Add(static_cast<int32>(NextRandom(state) % 1000));
        }
        values.Sort();
        bool sorted = true;
        for (usize i = 1; i < values.Count(); ++i)
        {
            sorted = sorted && values[i - 1] <= values[i];
        }
        MARLIN_EXPECT(sorted);
        struct Item
        {
            int32 Key;
            int32 Order;
        };
        List<Item> items;
        for (int32 i = 0; i < 5000; ++i)
        {
            items.Add({static_cast<int32>(NextRandom(state) % 50), i});
        }
        items.StableSort([](const Item &a, const Item &b) { return a.Key < b.Key; });
        bool stable = true;
        for (usize i = 1; i < items.Count(); ++i)
        {
            stable = stable && (items[i - 1].Key < items[i].Key ||
                                (items[i - 1].Key == items[i].Key && items[i - 1].Order < items[i].Order));
        }
        MARLIN_EXPECT(stable);
        MARLIN_EXPECT(LowerBound(values.Data(), values.Count(), 500) == UpperBound(values.Data(), values.Count(), 499));
        Span<const int32> span = values;
        MARLIN_EXPECT(span.Slice(10, 5).Count() == 5 && span.Right(3).Count() == 3);
    }
}
} // namespace Marlin::Tests
