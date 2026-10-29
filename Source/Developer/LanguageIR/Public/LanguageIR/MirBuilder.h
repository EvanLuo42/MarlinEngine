#pragma once

#include "LanguageIR/MirModule.h"

namespace Marlin
{
/// Creates instructions at an insertion point. Result types are derived from the operands where the operation
/// allows it; the verifier checks the rest.
class LANGUAGEIR_API MirBuilder
{
public:
    explicit MirBuilder(MirModule &module) : _module(&module), _types(&module.GetTypes())
    {
    }

    [[nodiscard]] MirModule &GetModule() const
    {
        return *_module;
    }

    [[nodiscard]] MirTypeContext &GetTypes() const
    {
        return *_types;
    }

    /// New instructions go to the end of *block*.
    void SetInsertPoint(MirBlock *block)
    {
        _block = block;
        _before = nullptr;
    }

    /// New instructions go before *instruction*.
    void SetInsertPoint(MirInstruction *instruction)
    {
        _block = instruction->GetParent();
        _before = instruction;
    }

    [[nodiscard]] MirBlock *GetBlock() const
    {
        return _block;
    }

    /// Whether the current block already ends in a terminator.
    [[nodiscard]] bool IsTerminated() const
    {
        return _block == nullptr || (_before == nullptr && _block->GetTerminator() != nullptr);
    }

    /// The source location given to new instructions.
    void SetLocation(MirLocation location)
    {
        _location = location;
    }

    [[nodiscard]] MirLocation GetLocation() const
    {
        return _location;
    }

    /// Whether new instructions are `precise`: floating-point results computed exactly as written.
    void SetPrecise(bool precise)
    {
        _precise = precise;
    }

    [[nodiscard]] bool IsPrecise() const
    {
        return _precise;
    }

    [[nodiscard]] MirInstruction *Create(MirOp op, const MirType *type, Span<MirValue *const> operands);

    [[nodiscard]] MirInstruction *Binary(MirOp op, MirValue *left, MirValue *right);
    /// A comparison; the result is bool, or a bool vector for vector operands.
    [[nodiscard]] MirInstruction *Compare(MirOp op, MirValue *left, MirValue *right);
    [[nodiscard]] MirInstruction *Unary(MirOp op, MirValue *operand);
    [[nodiscard]] MirInstruction *Convert(MirValue *value, const MirType *type);
    [[nodiscard]] MirInstruction *Bitcast(MirValue *value, const MirType *type);
    [[nodiscard]] MirInstruction *Construct(const MirType *type, Span<MirValue *const> elements);
    [[nodiscard]] MirInstruction *Extract(MirValue *composite, Span<const uint32> indices);
    [[nodiscard]] MirInstruction *Insert(MirValue *composite, MirValue *value, Span<const uint32> indices);
    [[nodiscard]] MirInstruction *ExtractDynamic(MirValue *vector, MirValue *index);
    [[nodiscard]] MirInstruction *InsertDynamic(MirValue *vector, MirValue *value, MirValue *index);
    /// Picks components of *first* then *second*, numbered across both.
    [[nodiscard]] MirInstruction *Shuffle(MirValue *first, MirValue *second, Span<const uint32> components);
    [[nodiscard]] MirInstruction *Select(MirValue *condition, MirValue *whenTrue, MirValue *whenFalse);

    /// A function-local variable, placed at the start of the entry block whatever the insertion point.
    [[nodiscard]] MirInstruction *Variable(const MirType *type, StringView name = {});
    [[nodiscard]] MirInstruction *Load(MirValue *pointer);
    MirInstruction *Store(MirValue *pointer, MirValue *value);
    [[nodiscard]] MirInstruction *AccessChain(MirValue *pointer, Span<MirValue *const> indices);
    /// The elements of the runtime array that ends the struct or buffer *pointer* points to.
    [[nodiscard]] MirInstruction *ArrayLength(MirValue *pointer);
    /// A pointer to the contents of buffer *handle*: a runtime array of its element, or of uint for a byte
    /// address buffer.
    [[nodiscard]] MirInstruction *BufferPointer(MirValue *handle);
    MirInstruction *Call(MirFunction *function, Span<MirValue *const> arguments);
    [[nodiscard]] MirInstruction *Phi(const MirType *type);
    MirInstruction *Intrinsic(MirIntrinsic intrinsic, const MirType *type, Span<MirValue *const> operands,
                              Span<const int64> literals = {});

    // Script operations (MarlinScript §14.2); script types are named by their index in the module's type table.

    [[nodiscard]] MirInstruction *New(const MirType *referenceType, uint32 scriptType);
    [[nodiscard]] MirInstruction *NewArray(const MirType *referenceType, uint32 scriptType, MirValue *length);
    /// A pointer to field *field* of the object, a *fieldType*, by the layout of the class *scriptType*.
    [[nodiscard]] MirInstruction *FieldAddress(MirValue *object, const MirType *fieldType, uint32 field, uint32 scriptType);
    [[nodiscard]] MirInstruction *ElementAddress(MirValue *array, MirValue *index);
    [[nodiscard]] MirInstruction *ObjectLength(MirValue *object);
    [[nodiscard]] MirInstruction *Box(MirValue *value, const MirType *referenceType, uint32 scriptType);
    [[nodiscard]] MirInstruction *Unbox(MirValue *object, const MirType *valueType, uint32 scriptType);
    [[nodiscard]] MirInstruction *IsInstance(MirValue *object, uint32 scriptType);
    [[nodiscard]] MirInstruction *Cast(MirValue *object, const MirType *referenceType, uint32 scriptType, bool orNull);
    [[nodiscard]] MirInstruction *TypeOf(const MirType *typeInfo, uint32 scriptType);
    [[nodiscard]] MirInstruction *NullCheck(MirValue *value, const MirType *type);
    MirInstruction *CallVirtual(MirFunction *declared, uint32 slot, Span<MirValue *const> arguments);
    MirInstruction *CallInterface(MirFunction *declared, uint32 interfaceType, uint32 method,
                                  Span<MirValue *const> arguments);
    /// Calls a delegate; *allowEmpty* makes a null delegate call nothing, as an event's does.
    MirInstruction *CallDelegate(MirValue *delegate, const MirType *result, Span<MirValue *const> arguments,
                                 bool allowEmpty = false);
    [[nodiscard]] MirInstruction *MakeDelegate(const MirType *delegateType, uint32 scriptType, MirFunction *function,
                                               MirValue *receiver, int64 slot);
    [[nodiscard]] MirInstruction *CombineDelegates(MirValue *left, MirValue *right, bool remove);
    [[nodiscard]] MirInstruction *StringConstant(const MirType *stringType, uint32 string);
    MirInstruction *Await(MirValue *task, const MirType *result);
    MirInstruction *Yield(MirValue *value);
    MirInstruction *Fault(MirFault fault, int64 message = -1);

    MirInstruction *Branch(MirBlock *target);
    MirInstruction *CondBranch(MirValue *condition, MirBlock *whenTrue, MirBlock *whenFalse);
    MirInstruction *Switch(MirValue *selector, MirBlock *defaultTarget, Span<const int64> cases,
                           Span<MirBlock *const> targets);
    MirInstruction *Return(MirValue *value = nullptr);
    MirInstruction *Terminate(MirOp op, Span<MirValue *const> operands = {});

private:
    MirInstruction *NewInstruction(MirOp op, const MirType *type);
    [[nodiscard]] const MirType *BoolLike(const MirType *type) const;

    MirModule *_module;
    MirTypeContext *_types;
    MirBlock *_block = nullptr;
    MirInstruction *_before = nullptr;
    MirLocation _location;
    bool _precise = false;
};

/// The type reached by indexing *type* with constant *indices*; null if they do not fit.
LANGUAGEIR_API const MirType *GetIndexedType(const MirType *type, Span<const uint32> indices);
} // namespace Marlin
