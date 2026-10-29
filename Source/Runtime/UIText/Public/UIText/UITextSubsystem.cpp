#include "UIText/UITextSubsystem.h"

#include "Asset/Assets.h"
#include "Concurrency/ScopeLock.h"
#include "Concurrency/ThreadLocal.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "String/Utf.h"

#include "hb-ot.h"
#include "hb.h"

MARLIN_DEFINE_LOG_CATEGORY(LogUIText);

extern "C" void *MarlinHarfBuzzMalloc(size_t size)
{
    return Marlin::Memory::Allocate(size != 0 ? size : 1);
}

extern "C" void *MarlinHarfBuzzCalloc(size_t count, size_t size)
{
    const size_t bytes = count * size;
    void *pointer = Marlin::Memory::Allocate(bytes != 0 ? bytes : 1);
    Marlin::MemZero(pointer, bytes);
    return pointer;
}

extern "C" void *MarlinHarfBuzzRealloc(void *pointer, size_t size)
{
    return Marlin::Memory::Reallocate(pointer, size != 0 ? size : 1);
}

extern "C" void MarlinHarfBuzzFree(void *pointer)
{
    Marlin::Memory::Free(pointer);
}

namespace Marlin
{
List<SoftAssetRef<Font>> UITextSubsystem::DefaultFonts;
SoftAssetRef<Font> UITextSubsystem::MonospaceFont;

struct UITextSubsystem::Face
{
    UIFontFace Id;
    AssetRef<Font> Font;
    AssetReloadId Reload = 0;
    hb_blob_t *Blob = nullptr;
    hb_face_t *HbFace = nullptr;
    hb_font_t *HbFont = nullptr;
    float32 UnitsPerEm = 1000;
    float32 Ascent = 0;
    float32 Descent = 0;
    float32 LineGap = 0;
};

namespace
{
struct OutlineSink
{
    UIPath *Path = nullptr;
    float32 Scale = 1;
};

void MoveTo(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float x, float y, void *)
{
    auto *sink = static_cast<OutlineSink *>(data);
    sink->Path->MoveTo({x * sink->Scale, -y * sink->Scale});
}

void LineTo(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float x, float y, void *)
{
    auto *sink = static_cast<OutlineSink *>(data);
    sink->Path->LineTo({x * sink->Scale, -y * sink->Scale});
}

void QuadTo(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float cx, float cy, float x, float y, void *)
{
    auto *sink = static_cast<OutlineSink *>(data);
    sink->Path->QuadTo({cx * sink->Scale, -cy * sink->Scale}, {x * sink->Scale, -y * sink->Scale});
}

void CubicTo(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float c1x, float c1y, float c2x, float c2y, float x,
             float y, void *)
{
    auto *sink = static_cast<OutlineSink *>(data);
    sink->Path->CubicTo({c1x * sink->Scale, -c1y * sink->Scale}, {c2x * sink->Scale, -c2y * sink->Scale},
                        {x * sink->Scale, -y * sink->Scale});
}

void ClosePath(hb_draw_funcs_t *, void *data, hb_draw_state_t *, void *)
{
    static_cast<OutlineSink *>(data)->Path->Close();
}

struct Character
{
    char32_t CodePoint = 0;
    uint32 Offset = 0;
    uint32 Length = 0;
};

/// What Shape works in, per thread, so that shaping a line reuses one HarfBuzz buffer and its lists. Lives until the
/// process exits.
struct ShapeScratch
{
    hb_buffer_t *Buffer = nullptr;
    List<Character> Characters;
    List<uint32> Chosen;
};

ThreadLocal<ShapeScratch> ShapeScratches;
} // namespace

void DrawText(UIPainter &painter, const UIShapedText &text, Vector2 origin, const UIBrush &brush)
{
    Vector2 pen = origin;
    for (const UIShapedRun &run : text.Runs)
    {
        painter.DrawText(run, pen, brush);
        pen += run.Advance;
    }
}

UITextSubsystem::UITextSubsystem() = default;
UITextSubsystem::~UITextSubsystem() = default;

void UITextSubsystem::Initialize(SubsystemContext & /*context*/)
{
    _drawFuncs = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(_drawFuncs, &MoveTo, nullptr, nullptr);
    hb_draw_funcs_set_line_to_func(_drawFuncs, &LineTo, nullptr, nullptr);
    hb_draw_funcs_set_quadratic_to_func(_drawFuncs, &QuadTo, nullptr, nullptr);
    hb_draw_funcs_set_cubic_to_func(_drawFuncs, &CubicTo, nullptr, nullptr);
    hb_draw_funcs_set_close_path_func(_drawFuncs, &ClosePath, nullptr, nullptr);
    hb_draw_funcs_make_immutable(_drawFuncs);
    for (const SoftAssetRef<Font> &font : DefaultFonts)
    {
        if (font.IsValid())
        {
            _defaultFaces.Add(RegisterFont(Assets::Load<Font>(font.GetGuid(), IoPriority::Critical)));
        }
    }
    if (MonospaceFont.IsValid())
    {
        _monospaceFaces.Add(RegisterFont(Assets::Load<Font>(MonospaceFont.GetGuid(), IoPriority::Critical)));
    }
    _monospaceFaces.AddRange(_defaultFaces.AsSpan());
    if (_defaultFaces.IsEmpty())
    {
        MARLIN_LOG(LogUIText, Warning, "Engine.UIText.DefaultFonts names no font; text draws nothing");
    }
}

void UITextSubsystem::Deinitialize()
{
    _lifetime->Alive = false;
    _lock.Lock();
    for (UniquePtr<Face> &face : _faces)
    {
        if (face->Reload != 0)
        {
            Assets::UnwatchReload(face->Reload);
        }
        Unload(*face);
    }
    _faces.Clear();
    _lock.Unlock();
    _defaultFaces.Clear();
    _monospaceFaces.Clear();
    hb_draw_funcs_destroy(_drawFuncs);
    _drawFuncs = nullptr;
}

UIFontFace UITextSubsystem::RegisterFont(const AssetRef<Font> &font)
{
    AssetSlot *slot = font.GetSlot();
    if (slot == nullptr)
    {
        return {};
    }
    _lock.LockShared();
    for (const UniquePtr<Face> &face : _faces)
    {
        if (face->Font.GetSlot() == slot)
        {
            const UIFontFace id = face->Id;
            _lock.UnlockShared();
            return id;
        }
    }
    _lock.UnlockShared();
    UniquePtr<Face> face = MakeUnique<Face>();
    face->Font = font;
    Face *entry = face.Get();
    _lock.Lock();
    _faces.Add(Move(face));
    entry->Id.Id = static_cast<uint32>(_faces.Count());
    _lock.Unlock();
    Assets::Load(*slot);
    Assets::OnCompleted(*slot, [this, entry, lifetime = _lifetime](AssetSlot &) {
        if (!lifetime->Alive)
        {
            return;
        }
        _lock.Lock();
        Load(*entry);
        _lock.Unlock();
    });
    entry->Reload = Assets::WatchReload(*slot, [this, entry](AssetSlot &) {
        _lock.Lock();
        Unload(*entry);
        Load(*entry);
        _lock.Unlock();
    });
    return entry->Id;
}

void UITextSubsystem::Load(Face &face)
{
    const Font *font = face.Font.Get();
    if (font == nullptr || face.HbFont != nullptr)
    {
        if (font == nullptr)
        {
            MARLIN_LOG(LogUIText, Warning, "Font {0} did not load", face.Font.GetGuid());
        }
        return;
    }
    face.Blob =
        hb_blob_create(reinterpret_cast<const char *>(font->Data.Data()), static_cast<unsigned int>(font->Data.Count()),
                       HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    face.HbFace = hb_face_create(face.Blob, font->FaceIndex);
    face.HbFont = hb_font_create(face.HbFace);
    const unsigned int upem = hb_face_get_upem(face.HbFace);
    face.UnitsPerEm = static_cast<float32>(upem != 0 ? upem : 1000);
    hb_font_set_scale(face.HbFont, static_cast<int>(face.UnitsPerEm), static_cast<int>(face.UnitsPerEm));
    hb_font_make_immutable(face.HbFont);
    hb_font_extents_t extents{};
    if (hb_font_get_h_extents(face.HbFont, &extents))
    {
        face.Ascent = static_cast<float32>(extents.ascender) / face.UnitsPerEm;
        face.Descent = static_cast<float32>(-extents.descender) / face.UnitsPerEm;
        face.LineGap = static_cast<float32>(extents.line_gap) / face.UnitsPerEm;
    }
    MARLIN_LOG(LogUIText, Debug, "Font {0}: {1} glyphs, {2} units per em", font->Name,
               hb_face_get_glyph_count(face.HbFace), face.UnitsPerEm);
}

void UITextSubsystem::Unload(Face &face)
{
    hb_font_destroy(face.HbFont);
    hb_face_destroy(face.HbFace);
    hb_blob_destroy(face.Blob);
    face.HbFont = nullptr;
    face.HbFace = nullptr;
    face.Blob = nullptr;
}

const UITextSubsystem::Face *UITextSubsystem::FindReady(UIFontFace face) const
{
    if (face.Id == 0 || face.Id > _faces.Count())
    {
        return nullptr;
    }
    const Face *entry = _faces[face.Id - 1].Get();
    return entry->HbFont != nullptr ? entry : nullptr;
}

bool UITextSubsystem::IsReady(UIFontFace face) const
{
    _lock.LockShared();
    const bool ready = FindReady(face) != nullptr;
    _lock.UnlockShared();
    return ready;
}

// RECOVERY-DECISION D2: the earlier surviving body; the final one was a line shorter (outline 261-363).
void UITextSubsystem::Shape(StringView text, const UITextStyle &style, UIShapedText &shaped) const
{
    shaped = {};
    _lock.LockShared();
    List<const Face *> faces;
    for (const UIFontFace face : style.Faces)
    {
        if (const Face *entry = FindReady(face))
        {
            faces.Add(entry);
        }
    }
    if (faces.IsEmpty() || text.IsEmpty())
    {
        _lock.UnlockShared();
        return;
    }
    shaped.Ascent = faces[0]->Ascent * style.Size;
    shaped.Descent = faces[0]->Descent * style.Size;
    shaped.LineGap = faces[0]->LineGap * style.Size;

    ShapeScratch &scratch = ShapeScratches.Get();
    List<Character> &characters = scratch.Characters;
    List<uint32> &chosen = scratch.Chosen;
    characters.Clear();
    chosen.Clear();
    const char *cursor = text.Data();
    const char *end = text.Data() + text.Length();
    while (cursor < end)
    {
        const char *start = cursor;
        const char32_t codePoint = Utf8::Decode(cursor, end);
        characters.Add({codePoint, static_cast<uint32>(start - text.Data()), static_cast<uint32>(cursor - start)});
        uint32 pick = 0;
        const bool joiner = codePoint == 0x200D || (codePoint >= 0xFE00 && codePoint <= 0xFE0F) ||
                            (codePoint >= 0x0300 && codePoint <= 0x036F);
        if (joiner && !chosen.IsEmpty())
        {
            pick = chosen.Last();
        }
        else
        {
            for (uint32 index = 0; index < faces.Count(); ++index)
            {
                hb_codepoint_t glyph = 0;
                if (hb_font_get_nominal_glyph(faces[index]->HbFont, codePoint, &glyph))
                {
                    pick = index;
                    break;
                }
            }
        }
        chosen.Add(pick);
    }

    if (scratch.Buffer == nullptr)
    {
        scratch.Buffer = hb_buffer_create();
    }
    hb_buffer_t *buffer = scratch.Buffer;
    usize first = 0;
    while (first < characters.Count())
    {
        usize last = first + 1;
        while (last < characters.Count() && chosen[last] == chosen[first])
        {
            ++last;
        }
        const Face &face = *faces[chosen[first]];
        const uint32 offset = characters[first].Offset;
        const uint32 length = characters[last - 1].Offset + characters[last - 1].Length - offset;
        hb_buffer_clear_contents(buffer);
        hb_buffer_add_utf8(buffer, text.Data(), static_cast<int>(text.Length()), offset, static_cast<int>(length));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(face.HbFont, buffer, nullptr, 0);
        unsigned int count = 0;
        const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, &count);
        const float32 scale = style.Size / face.UnitsPerEm;
        UIShapedRun &run = shaped.Runs.AddDefaulted();
        run.Face = face.Id;
        run.Size = style.Size;
        run.Ascent = face.Ascent * style.Size;
        run.Descent = face.Descent * style.Size;
        run.HasColorGlyphs = hb_ot_color_has_layers(face.HbFace) || hb_ot_color_has_paint(face.HbFace);
        Vector2 pen;
        for (unsigned int index = 0; index < count; ++index)
        {
            UIShapedGlyph &glyph = run.Glyphs.AddDefaulted();
            glyph.GlyphId = infos[index].codepoint;
            glyph.Position = {pen.X + static_cast<float32>(positions[index].x_offset) * scale,
                              pen.Y - static_cast<float32>(positions[index].y_offset) * scale};
            hb_glyph_extents_t extents{};
            if (hb_font_get_glyph_extents(face.HbFont, glyph.GlyphId, &extents) && extents.width != 0 &&
                extents.height != 0)
            {
                glyph.Bounds = UIRect(
                    static_cast<float32>(extents.x_bearing) * scale, -static_cast<float32>(extents.y_bearing) * scale,
                    static_cast<float32>(extents.width) * scale, -static_cast<float32>(extents.height) * scale);
            }
            pen.X += static_cast<float32>(positions[index].x_advance) * scale;
            pen.Y -= static_cast<float32>(positions[index].y_advance) * scale;
        }
        run.Advance = pen;
        shaped.Width += pen.X;
        first = last;
    }
    _lock.UnlockShared();
}

bool UITextSubsystem::GetGlyphOutline(UIFontFace face, uint32 glyph, UIPath &outline) const
{
    outline.Clear();
    _lock.LockShared();
    const Face *entry = FindReady(face);
    bool drawn = false;
    if (entry != nullptr)
    {
        OutlineSink sink{&outline, 1 / entry->UnitsPerEm};
        drawn = hb_font_draw_glyph_or_fail(entry->HbFont, glyph, _drawFuncs, &sink) && !outline.IsEmpty();
    }
    _lock.UnlockShared();
    return drawn;
}
} // namespace Marlin
