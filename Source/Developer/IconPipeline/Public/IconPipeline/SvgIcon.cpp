#include "IconPipeline/SvgIcon.h"

#include "Math/MathUtility.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/Number.h"
#include "Templates/Utility.h"
#include "UIPainter/UIGeometry.h"

namespace Marlin
{
namespace
{
/// The magic number for quarter circles drawn as cubic curves: 4/3 · (√2 − 1).
constexpr float32 Kappa = 0.5522847498f;

bool IsSeparator(char c)
{
    return Ascii::IsWhiteSpace(c) || c == ',';
}

void SkipSeparators(StringView text, usize &pos)
{
    while (pos < text.Length() && IsSeparator(text[pos]))
    {
        ++pos;
    }
}

/// Reads an SVG number at *pos*: optional sign, digits with an optional point, optional exponent. "1.5.5" is two
/// numbers and "1-2" is two numbers, as SVG path data allows.
bool ScanNumber(StringView text, usize &pos, float32 &value)
{
    const usize start = pos;
    usize i = pos;
    if (i < text.Length() && (text[i] == '+' || text[i] == '-'))
    {
        ++i;
    }
    bool digits = false;
    while (i < text.Length() && Ascii::IsDigit(text[i]))
    {
        ++i;
        digits = true;
    }
    usize end = i;
    if (i < text.Length() && text[i] == '.')
    {
        ++i;
        bool fraction = false;
        while (i < text.Length() && Ascii::IsDigit(text[i]))
        {
            ++i;
            fraction = true;
        }
        digits = digits || fraction;
        // "1." is a number; the parser is given it without the point.
        end = fraction ? i : end;
    }
    if (!digits)
    {
        return false;
    }
    if (i < text.Length() && (text[i] == 'e' || text[i] == 'E'))
    {
        usize j = i + 1;
        if (j < text.Length() && (text[j] == '+' || text[j] == '-'))
        {
            ++j;
        }
        if (j < text.Length() && Ascii::IsDigit(text[j]))
        {
            while (j < text.Length() && Ascii::IsDigit(text[j]))
            {
                ++j;
            }
            // Only valid when the mantissa has its digits in place; "1.e5" keeps its point dropped above.
            if (end == i)
            {
                end = j;
            }
            i = j;
        }
    }
    if (!TryParse(text.Substring(start, end - start), value))
    {
        return false;
    }
    pos = i;
    return true;
}

/// Every number of a whitespace- or comma-separated list; false if something else is in it.
bool ParseNumberList(StringView text, List<float32> &values)
{
    values.Clear();
    usize pos = 0;
    SkipSeparators(text, pos);
    while (pos < text.Length())
    {
        float32 value = 0;
        if (!ScanNumber(text, pos, value))
        {
            return false;
        }
        values.Add(value);
        SkipSeparators(text, pos);
    }
    return true;
}

/// The numbers a path command takes per repetition.
usize GetArgumentCount(char command)
{
    switch (command)
    {
    case 'M':
    case 'L':
    case 'T':
        return 2;
    case 'H':
    case 'V':
        return 1;
    case 'C':
        return 6;
    case 'S':
    case 'Q':
        return 4;
    case 'A':
        return 7;
    default:
        return 0;
    }
}

/// The part of an XML name after its namespace prefix.
StringView LocalName(StringView name)
{
    const usize colon = name.LastIndexOf(':');
    return colon == StringView::NotFound ? name : name.Substring(colon + 1);
}

struct XmlAttribute
{
    StringView Name;
    StringView Value;
};

struct XmlTag
{
    StringView Name;
    List<XmlAttribute> Attributes;
    bool Closing = false;
    bool SelfClosing = false;

    [[nodiscard]] const XmlAttribute *Find(StringView name) const
    {
        for (const XmlAttribute &attribute : Attributes)
        {
            if (attribute.Name == name)
            {
                return &attribute;
            }
        }
        return nullptr;
    }
};

/// Reads tags one at a time, skipping text, comments, processing instructions, CDATA and the doctype.
class XmlReader
{
public:
    enum class Token : uint8
    {
        Tag,
        End,
        Error,
    };

    explicit XmlReader(StringView text) : _text(text)
    {
    }

    Token Next(XmlTag &tag, String &error)
    {
        while (true)
        {
            const usize open = _text.IndexOf('<', _pos);
            if (open == StringView::NotFound)
            {
                _pos = _text.Length();
                return Token::End;
            }
            _pos = open;
            const StringView rest = _text.Substring(open);
            if (rest.StartsWith("<!--"))
            {
                if (!SkipPast("-->", error))
                {
                    return Token::Error;
                }
                continue;
            }
            if (rest.StartsWith("<?"))
            {
                if (!SkipPast("?>", error))
                {
                    return Token::Error;
                }
                continue;
            }
            if (rest.StartsWith("<![CDATA["))
            {
                if (!SkipPast("]]>", error))
                {
                    return Token::Error;
                }
                continue;
            }
            if (rest.StartsWith("<!"))
            {
                // A doctype, perhaps with an internal subset in brackets.
                const usize bracket = _text.IndexOf('[', _pos);
                const usize close = _text.IndexOf('>', _pos);
                const bool subset = bracket != StringView::NotFound && close != StringView::NotFound && bracket < close;
                if (!SkipPast(subset ? StringView("]>") : StringView(">"), error))
                {
                    return Token::Error;
                }
                continue;
            }
            return ReadTag(tag, error) ? Token::Tag : Token::Error;
        }
    }

    /// The 1-based line of the reader's position, for errors.
    [[nodiscard]] uint32 GetLine() const
    {
        uint32 line = 1;
        for (usize i = 0; i < _pos && i < _text.Length(); ++i)
        {
            line += _text[i] == '\n' ? 1u : 0u;
        }
        return line;
    }

private:
    bool SkipPast(StringView terminator, String &error)
    {
        const usize found = _text.IndexOf(terminator, _pos);
        if (found == StringView::NotFound)
        {
            error = Format("unterminated markup at line {0}", GetLine());
            return false;
        }
        _pos = found + terminator.Length();
        return true;
    }

    [[nodiscard]] static bool IsNameChar(char c)
    {
        return !Ascii::IsWhiteSpace(c) && c != '/' && c != '>' && c != '=' && c != '<' && c != '"' && c != '\'';
    }

    StringView ReadName()
    {
        const usize start = _pos;
        while (_pos < _text.Length() && IsNameChar(_text[_pos]))
        {
            ++_pos;
        }
        return _text.Substring(start, _pos - start);
    }

    void SkipWhiteSpace()
    {
        while (_pos < _text.Length() && Ascii::IsWhiteSpace(_text[_pos]))
        {
            ++_pos;
        }
    }

    bool ReadTag(XmlTag &tag, String &error)
    {
        tag.Attributes.Clear();
        tag.Closing = false;
        tag.SelfClosing = false;
        ++_pos;
        if (_pos < _text.Length() && _text[_pos] == '/')
        {
            tag.Closing = true;
            ++_pos;
        }
        tag.Name = ReadName();
        if (tag.Name.IsEmpty())
        {
            error = Format("malformed tag at line {0}", GetLine());
            return false;
        }
        while (true)
        {
            SkipWhiteSpace();
            if (_pos >= _text.Length())
            {
                error = Format("unterminated tag <{0}> at line {1}", tag.Name, GetLine());
                return false;
            }
            const char c = _text[_pos];
            if (c == '>')
            {
                ++_pos;
                return true;
            }
            if (c == '/' && _pos + 1 < _text.Length() && _text[_pos + 1] == '>' && !tag.Closing)
            {
                tag.SelfClosing = true;
                _pos += 2;
                return true;
            }
            if (tag.Closing)
            {
                error = Format("malformed closing tag </{0}> at line {1}", tag.Name, GetLine());
                return false;
            }
            XmlAttribute attribute;
            attribute.Name = ReadName();
            SkipWhiteSpace();
            if (attribute.Name.IsEmpty() || _pos >= _text.Length() || _text[_pos] != '=')
            {
                error = Format("malformed attribute in <{0}> at line {1}", tag.Name, GetLine());
                return false;
            }
            ++_pos;
            SkipWhiteSpace();
            if (_pos >= _text.Length() || (_text[_pos] != '"' && _text[_pos] != '\''))
            {
                error = Format("unquoted attribute {0} in <{1}> at line {2}", attribute.Name, tag.Name, GetLine());
                return false;
            }
            const char quote = _text[_pos++];
            const usize end = _text.IndexOf(quote, _pos);
            if (end == StringView::NotFound)
            {
                error = Format("unterminated attribute {0} at line {1}", attribute.Name, GetLine());
                return false;
            }
            attribute.Value = _text.Substring(_pos, end - _pos);
            _pos = end + 1;
            tag.Attributes.Add(attribute);
        }
    }

    StringView _text;
    usize _pos = 0;
};

enum class PaintKind : uint8
{
    None,
    CurrentColor,
    Color,
};

struct Paint
{
    PaintKind Kind = PaintKind::None;
    /// Linear, straight alpha.
    LinearColor Color;
};

/// The inherited state at an element, SVG's initial values at the root.
struct SvgStyle
{
    Paint Fill = {PaintKind::Color, LinearColor::Black};
    Paint Stroke;
    float32 StrokeWidth = 1;
    UILineCap Cap = UILineCap::Butt;
    UILineJoin Join = UILineJoin::Miter;
    float32 MiterLimit = 4;
    UIFillRule FillRule = UIFillRule::NonZero;
    /// The product of the element's and its ancestors' opacity: group opacity is approximated per shape.
    float32 Opacity = 1;
    float32 FillOpacity = 1;
    float32 StrokeOpacity = 1;
    bool Visible = true;
    /// From the element's user space to the view box's, whose top-left corner is the origin.
    UITransform Transform;
};

struct Frame
{
    SvgStyle Style;
    /// Nothing inside is drawn: an unsupported or hidden element.
    bool Skip = false;
};

struct NamedColor
{
    StringView Name;
    uint32 Rgb;
};

constexpr NamedColor NamedColors[] = {
    {"black", 0x000000},  {"white", 0xFFFFFF}, {"red", 0xFF0000},    {"green", 0x008000},   {"blue", 0x0000FF},
    {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF},  {"aqua", 0x00FFFF},   {"magenta", 0xFF00FF}, {"fuchsia", 0xFF00FF},
    {"gray", 0x808080},   {"grey", 0x808080},  {"silver", 0xC0C0C0}, {"maroon", 0x800000},  {"navy", 0x000080},
    {"purple", 0x800080}, {"teal", 0x008080},  {"olive", 0x808000},  {"lime", 0x00FF00},    {"orange", 0xFFA500},
};

LinearColor FromSrgb(float32 r, float32 g, float32 b, float32 a)
{
    return {LinearColor::SrgbToLinear(Clamp(r, 0.0f, 1.0f)), LinearColor::SrgbToLinear(Clamp(g, 0.0f, 1.0f)),
            LinearColor::SrgbToLinear(Clamp(b, 0.0f, 1.0f)), Clamp(a, 0.0f, 1.0f)};
}

/// #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), transparent and the named colors.
bool ParseColor(StringView text, LinearColor &color)
{
    if (text.StartsWith('#'))
    {
        const StringView hex = text.Substring(1);
        uint32 value = 0;
        for (usize i = 0; i < hex.Length(); ++i)
        {
            if (!Ascii::IsHexDigit(hex[i]))
            {
                return false;
            }
            value = (value << 4) | static_cast<uint32>(Ascii::DigitValue(hex[i]));
        }
        const auto nibble = [&](uint32 index, uint32 count) {
            return static_cast<float32>((value >> ((count - 1 - index) * 4)) & 0xF) / 15.0f;
        };
        const auto byte = [&](uint32 index, uint32 count) {
            return static_cast<float32>((value >> ((count - 1 - index) * 8)) & 0xFF) / 255.0f;
        };
        switch (hex.Length())
        {
        case 3:
            color = FromSrgb(nibble(0, 3), nibble(1, 3), nibble(2, 3), 1);
            return true;
        case 4:
            color = FromSrgb(nibble(0, 4), nibble(1, 4), nibble(2, 4), nibble(3, 4));
            return true;
        case 6:
            color = FromSrgb(byte(0, 3), byte(1, 3), byte(2, 3), 1);
            return true;
        case 8:
            color = FromSrgb(byte(0, 4), byte(1, 4), byte(2, 4), byte(3, 4));
            return true;
        default:
            return false;
        }
    }
    if (text.StartsWith("rgb(") || text.StartsWith("rgba("))
    {
        if (!text.EndsWith(')'))
        {
            return false;
        }
        const usize open = text.IndexOf('(');
        StringView arguments = text.Substring(open + 1, text.Length() - open - 2);
        float32 channels[4] = {0, 0, 0, 1};
        usize count = 0;
        usize pos = 0;
        SkipSeparators(arguments, pos);
        while (pos < arguments.Length())
        {
            float32 value = 0;
            if (count == 4 || !ScanNumber(arguments, pos, value))
            {
                return false;
            }
            if (pos < arguments.Length() && arguments[pos] == '%')
            {
                value = value / 100.0f * (count < 3 ? 255.0f : 1.0f);
                ++pos;
            }
            channels[count++] = value;
            SkipSeparators(arguments, pos);
            if (pos < arguments.Length() && arguments[pos] == '/')
            {
                ++pos;
                SkipSeparators(arguments, pos);
            }
        }
        if (count < 3)
        {
            return false;
        }
        color = FromSrgb(channels[0] / 255.0f, channels[1] / 255.0f, channels[2] / 255.0f, channels[3]);
        return true;
    }
    if (text.EqualsIgnoreCase("transparent"))
    {
        color = LinearColor::Transparent;
        return true;
    }
    for (const NamedColor &named : NamedColors)
    {
        if (text.EqualsIgnoreCase(named.Name))
        {
            color = FromSrgb(static_cast<float32>((named.Rgb >> 16) & 0xFF) / 255.0f,
                             static_cast<float32>((named.Rgb >> 8) & 0xFF) / 255.0f,
                             static_cast<float32>(named.Rgb & 0xFF) / 255.0f, 1);
            return true;
        }
    }
    return false;
}

/// A number, optionally in px, which is what user units are.
bool ParseLength(StringView text, float32 &value)
{
    text = text.Trim();
    if (text.EndsWith("px"))
    {
        text = text.RemoveEnd(2).TrimEnd();
    }
    usize pos = 0;
    return ScanNumber(text, pos, value) && pos == text.Length();
}

class SvgParser
{
public:
    SvgParser(Icon &icon, SvgIconDiagnostics &diagnostics) : _icon(icon), _diagnostics(diagnostics)
    {
    }

    bool Parse(StringView text)
    {
        XmlReader reader(text);
        XmlTag tag;
        bool sawRoot = false;
        while (true)
        {
            const XmlReader::Token token = reader.Next(tag, _diagnostics.Error);
            if (token == XmlReader::Token::Error)
            {
                return false;
            }
            if (token == XmlReader::Token::End)
            {
                break;
            }
            const StringView name = LocalName(tag.Name);
            if (tag.Closing)
            {
                if (!_frames.IsEmpty())
                {
                    _frames.Pop();
                }
                if (_frames.IsEmpty() && sawRoot)
                {
                    break;
                }
                continue;
            }
            if (!sawRoot)
            {
                if (name != "svg")
                {
                    _diagnostics.Error = Format("the root element is <{0}>, not <svg>", tag.Name);
                    return false;
                }
                sawRoot = true;
                if (!ReadRoot(tag))
                {
                    return false;
                }
                if (tag.SelfClosing)
                {
                    break;
                }
                continue;
            }
            if (name == "svg")
            {
                Warn("a nested <svg> is drawn as a group, without its own viewport");
            }
            Frame frame = _frames.IsEmpty() ? Frame() : _frames.Last();
            if (!frame.Skip)
            {
                ReadElement(name, tag, frame);
            }
            if (!tag.SelfClosing)
            {
                _frames.Add(frame);
            }
        }
        if (!sawRoot)
        {
            _diagnostics.Error = "the file holds no <svg> element";
            return false;
        }
        if (_shapes.IsEmpty())
        {
            Warn("the icon draws nothing");
        }
        _icon.Shapes = FrozenArray<IconShape>(_shapes.AsSpan());
        _icon.Verbs = FrozenArray<IconPathVerb>(_verbs.AsSpan());
        _icon.Points = FrozenArray<Vector2>(_points.AsSpan());
        return true;
    }

private:
    void Warn(StringView message)
    {
        for (const String &warning : _diagnostics.Warnings)
        {
            if (warning.AsView() == message)
            {
                return;
            }
        }
        _diagnostics.Warnings.Add(String(message));
    }

    bool ReadRoot(const XmlTag &tag)
    {
        Frame frame;
        Vector2 origin;
        if (const XmlAttribute *viewBox = tag.Find("viewBox"))
        {
            List<float32> values;
            if (!ParseNumberList(viewBox->Value, values) || values.Count() != 4 || !(values[2] > 0) || !(values[3] > 0))
            {
                _diagnostics.Error =
                    Format("the viewBox \"{0}\" is not four numbers with a positive size", viewBox->Value);
                return false;
            }
            origin = {values[0], values[1]};
            _icon.Size = {values[2], values[3]};
        }
        else
        {
            const XmlAttribute *width = tag.Find("width");
            const XmlAttribute *height = tag.Find("height");
            float32 w = 0;
            float32 h = 0;
            if (width == nullptr || height == nullptr || !ParseLength(width->Value, w) ||
                !ParseLength(height->Value, h) || !(w > 0) || !(h > 0))
            {
                _diagnostics.Error = "the <svg> element has neither a viewBox nor a width and height in user units";
                return false;
            }
            _icon.Size = {w, h};
        }
        if (const XmlAttribute *aspect = tag.Find("preserveAspectRatio"))
        {
            if (aspect->Value.Trim() != "xMidYMid meet" && aspect->Value.Trim() != "xMidYMid")
            {
                Warn("preserveAspectRatio is not supported; icons are always fitted and centred");
            }
        }
        ReadElement("svg", tag, frame);
        frame.Style.Transform = frame.Style.Transform.Then(UITransform::Translation(-origin.X, -origin.Y));
        _frames.Add(frame);
        return true;
    }

    void ReadElement(StringView name, const XmlTag &tag, Frame &frame)
    {
        if (name == "title" || name == "desc" || name == "metadata" || name == "defs")
        {
            // Not drawn; anything that refers into defs warns where it refers.
            frame.Skip = true;
            return;
        }
        const bool container = name == "g" || name == "svg";
        const bool shape = name == "path" || name == "circle" || name == "ellipse" || name == "rect" ||
                           name == "line" || name == "polyline" || name == "polygon";
        if (!container && !shape)
        {
            Warn(Format("<{0}> is not supported and is skipped", name).AsView());
            frame.Skip = true;
            return;
        }
        ReadStyle(tag, frame);
        if (frame.Skip || !shape)
        {
            // A shape's children (title and the like) are never drawn.
            return;
        }
        const uint32 firstVerb = static_cast<uint32>(_verbs.Count());
        const uint32 firstPoint = static_cast<uint32>(_points.Count());
        _transform = frame.Style.Transform;
        if (name == "path")
        {
            if (const XmlAttribute *data = tag.Find("d"))
            {
                ReadPathData(data->Value);
            }
        }
        else if (name == "circle")
        {
            const float32 r = GetLength(tag, "r");
            AddEllipse({GetLength(tag, "cx"), GetLength(tag, "cy")}, {r, r});
        }
        else if (name == "ellipse")
        {
            AddEllipse({GetLength(tag, "cx"), GetLength(tag, "cy")}, {GetLength(tag, "rx"), GetLength(tag, "ry")});
        }
        else if (name == "rect")
        {
            ReadRect(tag);
        }
        else if (name == "line")
        {
            MoveTo({GetLength(tag, "x1"), GetLength(tag, "y1")});
            LineTo({GetLength(tag, "x2"), GetLength(tag, "y2")});
        }
        else
        {
            ReadPoints(tag, name == "polygon");
        }
        AddShapes(frame.Style, firstVerb, firstPoint, name != "line");
        frame.Skip = true;
    }

    float32 GetLength(const XmlTag &tag, StringView name)
    {
        const XmlAttribute *attribute = tag.Find(name);
        float32 value = 0;
        if (attribute != nullptr && !ParseLength(attribute->Value, value))
        {
            Warn(Format("{0}=\"{1}\" is not a length in user units; 0 is used", name, attribute->Value).AsView());
            return 0;
        }
        return value;
    }

    void ReadStyle(const XmlTag &tag, Frame &frame)
    {
        for (const XmlAttribute &attribute : tag.Attributes)
        {
            if (attribute.Name == "transform")
            {
                UITransform transform;
                if (ParseTransform(attribute.Value, transform))
                {
                    frame.Style.Transform = transform.Then(frame.Style.Transform);
                }
                else
                {
                    Warn(Format("transform=\"{0}\" is malformed and is ignored", attribute.Value).AsView());
                }
            }
            else if (attribute.Name != "style")
            {
                ReadProperty(attribute.Name, attribute.Value, frame);
            }
        }
        // The style attribute wins over presentation attributes.
        if (const XmlAttribute *style = tag.Find("style"))
        {
            StringView rest = style->Value;
            while (!rest.IsEmpty())
            {
                StringView declaration = rest;
                StringView after;
                if (!rest.SplitFirst(';', declaration, after))
                {
                    after = {};
                }
                rest = after;
                StringView property;
                StringView value;
                if (declaration.SplitFirst(':', property, value))
                {
                    ReadProperty(property.Trim(), value.Trim(), frame);
                }
            }
        }
    }

    void ReadProperty(StringView name, StringView value, Frame &frame)
    {
        value = value.Trim();
        if (value == "inherit")
        {
            return;
        }
        SvgStyle &style = frame.Style;
        if (name == "fill" || name == "stroke")
        {
            ReadPaint(name, value, name == "fill" ? style.Fill : style.Stroke);
        }
        else if (name == "stroke-width")
        {
            float32 width = 0;
            if (ParseLength(value, width) && width >= 0)
            {
                style.StrokeWidth = width;
            }
            else
            {
                Warn(Format("stroke-width \"{0}\" is not a length in user units and is ignored", value).AsView());
            }
        }
        else if (name == "stroke-linecap")
        {
            if (value == "butt")
            {
                style.Cap = UILineCap::Butt;
            }
            else if (value == "round")
            {
                style.Cap = UILineCap::Round;
            }
            else if (value == "square")
            {
                style.Cap = UILineCap::Square;
            }
            else
            {
                Warn(Format("stroke-linecap \"{0}\" is not supported", value).AsView());
            }
        }
        else if (name == "stroke-linejoin")
        {
            if (value == "miter" || value == "miter-clip")
            {
                style.Join = UILineJoin::Miter;
            }
            else if (value == "round")
            {
                style.Join = UILineJoin::Round;
            }
            else if (value == "bevel")
            {
                style.Join = UILineJoin::Bevel;
            }
            else
            {
                Warn(Format("stroke-linejoin \"{0}\" is not supported", value).AsView());
            }
        }
        else if (name == "stroke-miterlimit")
        {
            float32 limit = 0;
            if (ParseLength(value, limit) && limit >= 1)
            {
                style.MiterLimit = limit;
            }
        }
        else if (name == "fill-rule")
        {
            if (value == "nonzero")
            {
                style.FillRule = UIFillRule::NonZero;
            }
            else if (value == "evenodd")
            {
                style.FillRule = UIFillRule::EvenOdd;
            }
        }
        else if (name == "opacity" || name == "fill-opacity" || name == "stroke-opacity")
        {
            float32 opacity = 1;
            usize pos = 0;
            if (!ScanNumber(value, pos, opacity))
            {
                return;
            }
            if (pos < value.Length() && value[pos] == '%')
            {
                opacity /= 100;
            }
            opacity = Clamp(opacity, 0.0f, 1.0f);
            if (name == "opacity")
            {
                style.Opacity *= opacity;
            }
            else
            {
                (name == "fill-opacity" ? style.FillOpacity : style.StrokeOpacity) = opacity;
            }
        }
        else if (name == "display")
        {
            frame.Skip = frame.Skip || value == "none";
        }
        else if (name == "visibility")
        {
            style.Visible = value == "visible";
        }
        else if (name == "stroke-dasharray")
        {
            if (value != "none")
            {
                Warn("stroke-dasharray is not supported; strokes are drawn solid");
            }
        }
        else if (name == "clip-path" || name == "mask" || name == "filter" || name == "marker-start" ||
                 name == "marker-mid" || name == "marker-end" || name == "vector-effect")
        {
            if (value != "none")
            {
                Warn(Format("{0} is not supported and is ignored", name).AsView());
            }
        }
    }

    void ReadPaint(StringView property, StringView value, Paint &paint)
    {
        if (value == "none")
        {
            paint.Kind = PaintKind::None;
            return;
        }
        if (value.EqualsIgnoreCase("currentColor"))
        {
            paint.Kind = PaintKind::CurrentColor;
            return;
        }
        if (value.StartsWith("url("))
        {
            Warn("paint servers (gradients and patterns) are not supported; their fallback color is used");
            const usize close = value.IndexOf(')');
            const StringView fallback =
                close == StringView::NotFound ? StringView() : value.Substring(close + 1).Trim();
            if (!fallback.IsEmpty() && fallback != value)
            {
                ReadPaint(property, fallback, paint);
            }
            else
            {
                paint.Kind = PaintKind::None;
            }
            return;
        }
        LinearColor color;
        if (!ParseColor(value, color))
        {
            Warn(Format("{0} color \"{1}\" is not supported and is ignored", property, value).AsView());
            return;
        }
        paint.Kind = PaintKind::Color;
        paint.Color = color;
    }

    static bool ParseTransform(StringView text, UITransform &result)
    {
        result = {};
        usize pos = 0;
        while (true)
        {
            SkipSeparators(text, pos);
            if (pos >= text.Length())
            {
                return true;
            }
            const usize nameStart = pos;
            while (pos < text.Length() && Ascii::IsLetter(text[pos]))
            {
                ++pos;
            }
            const StringView name = text.Substring(nameStart, pos - nameStart);
            while (pos < text.Length() && Ascii::IsWhiteSpace(text[pos]))
            {
                ++pos;
            }
            if (pos >= text.Length() || text[pos] != '(')
            {
                return false;
            }
            const usize close = text.IndexOf(')', pos);
            if (close == StringView::NotFound)
            {
                return false;
            }
            List<float32> a;
            if (!ParseNumberList(text.Substring(pos + 1, close - pos - 1), a))
            {
                return false;
            }
            pos = close + 1;
            UITransform item;
            const usize n = a.Count();
            if (name == "matrix" && n == 6)
            {
                item = {a[0], a[1], a[2], a[3], a[4], a[5]};
            }
            else if (name == "translate" && (n == 1 || n == 2))
            {
                item = UITransform::Translation(a[0], n == 2 ? a[1] : 0);
            }
            else if (name == "scale" && (n == 1 || n == 2))
            {
                item = UITransform::Scale(a[0], n == 2 ? a[1] : a[0]);
            }
            else if (name == "rotate" && (n == 1 || n == 3))
            {
                float32 sine = 0;
                float32 cosine = 1;
                Math::SinCos(a[0] * Math::DegreesToRadians, sine, cosine);
                item = {cosine, sine, -sine, cosine, 0, 0};
                if (n == 3)
                {
                    item = UITransform::Translation(-a[1], -a[2]).Then(item).Then(UITransform::Translation(a[1], a[2]));
                }
            }
            else if (name == "skewX" && n == 1)
            {
                item = {1, 0, Math::Tan(a[0] * Math::DegreesToRadians), 1, 0, 0};
            }
            else if (name == "skewY" && n == 1)
            {
                item = {1, Math::Tan(a[0] * Math::DegreesToRadians), 0, 1, 0, 0};
            }
            else
            {
                return false;
            }
            // "A B" applies B first, then A.
            result = item.Then(result);
        }
    }

    // Path output, in the view box's space.

    void MoveTo(Vector2 point)
    {
        _verbs.Add(IconPathVerb::MoveTo);
        _points.Add(_transform.Apply(point));
    }

    void LineTo(Vector2 point)
    {
        _verbs.Add(IconPathVerb::LineTo);
        _points.Add(_transform.Apply(point));
    }

    void QuadTo(Vector2 control, Vector2 point)
    {
        _verbs.Add(IconPathVerb::QuadTo);
        _points.Add(_transform.Apply(control));
        _points.Add(_transform.Apply(point));
    }

    void CubicTo(Vector2 control1, Vector2 control2, Vector2 point)
    {
        _verbs.Add(IconPathVerb::CubicTo);
        _points.Add(_transform.Apply(control1));
        _points.Add(_transform.Apply(control2));
        _points.Add(_transform.Apply(point));
    }

    void Close()
    {
        _verbs.Add(IconPathVerb::Close);
    }

    /// SVG's elliptical arc from *from* to *to* (SVG 1.1 implementation notes F.6.5), as cubic curves of at most a
    /// quarter turn each.
    void ArcTo(Vector2 from, Vector2 radii, float32 rotation, bool large, bool sweep, Vector2 to)
    {
        if (from == to)
        {
            return;
        }
        float32 rx = Math::Abs(radii.X);
        float32 ry = Math::Abs(radii.Y);
        if (rx == 0 || ry == 0)
        {
            LineTo(to);
            return;
        }
        float32 sine = 0;
        float32 cosine = 1;
        Math::SinCos(rotation * Math::DegreesToRadians, sine, cosine);
        const Vector2 half = (from - to) * 0.5f;
        const float32 x1 = cosine * half.X + sine * half.Y;
        const float32 y1 = -sine * half.X + cosine * half.Y;
        const float32 lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1)
        {
            const float32 grow = Math::Sqrt(lambda);
            rx *= grow;
            ry *= grow;
        }
        const float32 numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const float32 denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        float32 coefficient = denominator > 0 ? Math::Sqrt(Max(numerator / denominator, 0.0f)) : 0;
        if (large == sweep)
        {
            coefficient = -coefficient;
        }
        const float32 cx1 = coefficient * rx * y1 / ry;
        const float32 cy1 = -coefficient * ry * x1 / rx;
        const Vector2 center = {cosine * cx1 - sine * cy1 + (from.X + to.X) * 0.5f,
                                sine * cx1 + cosine * cy1 + (from.Y + to.Y) * 0.5f};
        const auto angle = [](Vector2 u, Vector2 v) {
            return Math::Atan2(u.X * v.Y - u.Y * v.X, u.X * v.X + u.Y * v.Y);
        };
        const Vector2 u = {(x1 - cx1) / rx, (y1 - cy1) / ry};
        const Vector2 v = {(-x1 - cx1) / rx, (-y1 - cy1) / ry};
        const float32 start = angle({1, 0}, u);
        float32 sweepAngle = angle(u, v);
        if (!sweep && sweepAngle > 0)
        {
            sweepAngle -= Math::TwoPi;
        }
        else if (sweep && sweepAngle < 0)
        {
            sweepAngle += Math::TwoPi;
        }
        const auto place = [&](float32 x, float32 y) -> Vector2 {
            return {center.X + cosine * rx * x - sine * ry * y, center.Y + sine * rx * x + cosine * ry * y};
        };
        const uint32 segments = Max(1u, static_cast<uint32>(Math::Abs(sweepAngle) / Math::HalfPi + 0.999f));
        const float32 step = sweepAngle / static_cast<float32>(segments);
        const float32 handle = 4.0f / 3.0f * Math::Tan(step / 4);
        for (uint32 i = 0; i < segments; ++i)
        {
            const float32 a1 = start + step * static_cast<float32>(i);
            const float32 a2 = a1 + step;
            float32 s1 = 0;
            float32 c1 = 1;
            float32 s2 = 0;
            float32 c2 = 1;
            Math::SinCos(a1, s1, c1);
            Math::SinCos(a2, s2, c2);
            CubicTo(place(c1 - handle * s1, s1 + handle * c1), place(c2 + handle * s2, s2 - handle * c2),
                    i + 1 == segments ? to : place(c2, s2));
        }
    }

    void AddEllipse(Vector2 center, Vector2 radii)
    {
        if (!(radii.X > 0) || !(radii.Y > 0))
        {
            return;
        }
        const float32 kx = radii.X * Kappa;
        const float32 ky = radii.Y * Kappa;
        const float32 x = center.X;
        const float32 y = center.Y;
        MoveTo({x + radii.X, y});
        CubicTo({x + radii.X, y + ky}, {x + kx, y + radii.Y}, {x, y + radii.Y});
        CubicTo({x - kx, y + radii.Y}, {x - radii.X, y + ky}, {x - radii.X, y});
        CubicTo({x - radii.X, y - ky}, {x - kx, y - radii.Y}, {x, y - radii.Y});
        CubicTo({x + kx, y - radii.Y}, {x + radii.X, y - ky}, {x + radii.X, y});
        Close();
    }

    void ReadRect(const XmlTag &tag)
    {
        const float32 x = GetLength(tag, "x");
        const float32 y = GetLength(tag, "y");
        const float32 w = GetLength(tag, "width");
        const float32 h = GetLength(tag, "height");
        if (!(w > 0) || !(h > 0))
        {
            return;
        }
        const bool hasRx = tag.Find("rx") != nullptr;
        const bool hasRy = tag.Find("ry") != nullptr;
        float32 rx = GetLength(tag, "rx");
        float32 ry = GetLength(tag, "ry");
        rx = hasRx ? rx : ry;
        ry = hasRy ? ry : rx;
        rx = Clamp(rx, 0.0f, w * 0.5f);
        ry = Clamp(ry, 0.0f, h * 0.5f);
        if (rx == 0 || ry == 0)
        {
            MoveTo({x, y});
            LineTo({x + w, y});
            LineTo({x + w, y + h});
            LineTo({x, y + h});
            Close();
            return;
        }
        const float32 kx = rx * Kappa;
        const float32 ky = ry * Kappa;
        const float32 r = x + w;
        const float32 b = y + h;
        MoveTo({x + rx, y});
        LineTo({r - rx, y});
        CubicTo({r - rx + kx, y}, {r, y + ry - ky}, {r, y + ry});
        LineTo({r, b - ry});
        CubicTo({r, b - ry + ky}, {r - rx + kx, b}, {r - rx, b});
        LineTo({x + rx, b});
        CubicTo({x + rx - kx, b}, {x, b - ry + ky}, {x, b - ry});
        LineTo({x, y + ry});
        CubicTo({x, y + ry - ky}, {x + rx - kx, y}, {x + rx, y});
        Close();
    }

    void ReadPoints(const XmlTag &tag, bool close)
    {
        const XmlAttribute *attribute = tag.Find("points");
        if (attribute == nullptr)
        {
            return;
        }
        List<float32> values;
        usize pos = 0;
        SkipSeparators(attribute->Value, pos);
        float32 value = 0;
        while (pos < attribute->Value.Length() && ScanNumber(attribute->Value, pos, value))
        {
            values.Add(value);
            SkipSeparators(attribute->Value, pos);
        }
        if (pos < attribute->Value.Length() || values.Count() % 2 != 0)
        {
            Warn("a points list is malformed; it is drawn up to the error");
        }
        const usize count = values.Count() / 2;
        if (count < 2)
        {
            return;
        }
        MoveTo({values[0], values[1]});
        for (usize i = 1; i < count; ++i)
        {
            LineTo({values[i * 2], values[i * 2 + 1]});
        }
        if (close)
        {
            Close();
        }
    }

    /// Reads *count* numbers of a command's argument set; arc flags are single characters.
    static bool ReadArguments(StringView data, usize &pos, float32 *values, usize count, bool arc)
    {
        for (usize i = 0; i < count; ++i)
        {
            SkipSeparators(data, pos);
            if (arc && (i == 3 || i == 4))
            {
                if (pos >= data.Length() || (data[pos] != '0' && data[pos] != '1'))
                {
                    return false;
                }
                values[i] = data[pos] == '1' ? 1.0f : 0.0f;
                ++pos;
                continue;
            }
            if (!ScanNumber(data, pos, values[i]))
            {
                return false;
            }
        }
        return true;
    }

    void ReadPathData(StringView data)
    {
        usize pos = 0;
        char command = 0;
        char previous = 0;
        Vector2 current;
        Vector2 start;
        Vector2 cubicControl;
        Vector2 quadControl;
        bool needsMove = true;
        bool malformed = false;
        while (true)
        {
            SkipSeparators(data, pos);
            if (pos >= data.Length())
            {
                break;
            }
            const char c = data[pos];
            if (Ascii::IsLetter(c))
            {
                const char upper = Ascii::ToUpper(c);
                if (StringView("MLHVCSQTAZ").IndexOf(upper) == StringView::NotFound || (command == 0 && upper != 'M'))
                {
                    malformed = true;
                    break;
                }
                command = c;
                ++pos;
            }
            else if (command == 0 || Ascii::ToUpper(command) == 'Z')
            {
                malformed = true;
                break;
            }
            const bool relative = Ascii::IsLower(command);
            const char upper = Ascii::ToUpper(command);
            const Vector2 base = relative ? current : Vector2();
            if (upper == 'Z')
            {
                if (!needsMove)
                {
                    Close();
                }
                current = start;
                needsMove = true;
                previous = 'Z';
                continue;
            }
            float32 a[7] = {};
            if (!ReadArguments(data, pos, a, GetArgumentCount(upper), upper == 'A'))
            {
                malformed = true;
                break;
            }
            if (upper == 'M')
            {
                current = base + Vector2(a[0], a[1]);
                start = current;
                MoveTo(current);
                needsMove = false;
                // Further pairs are lines.
                command = relative ? 'l' : 'L';
                previous = 'M';
                continue;
            }
            if (needsMove)
            {
                MoveTo(start);
                needsMove = false;
            }
            Vector2 next;
            switch (upper)
            {
            case 'L':
                next = base + Vector2(a[0], a[1]);
                LineTo(next);
                break;
            case 'H':
                next = {(relative ? current.X : 0) + a[0], current.Y};
                LineTo(next);
                break;
            case 'V':
                next = {current.X, (relative ? current.Y : 0) + a[0]};
                LineTo(next);
                break;
            case 'C':
                cubicControl = base + Vector2(a[2], a[3]);
                next = base + Vector2(a[4], a[5]);
                CubicTo(base + Vector2(a[0], a[1]), cubicControl, next);
                break;
            case 'S': {
                const bool smooth = previous == 'C' || previous == 'S';
                const Vector2 first = smooth ? current * 2.0f - cubicControl : current;
                cubicControl = base + Vector2(a[0], a[1]);
                next = base + Vector2(a[2], a[3]);
                CubicTo(first, cubicControl, next);
                break;
            }
            case 'Q':
                quadControl = base + Vector2(a[0], a[1]);
                next = base + Vector2(a[2], a[3]);
                QuadTo(quadControl, next);
                break;
            case 'T': {
                const bool smooth = previous == 'Q' || previous == 'T';
                quadControl = smooth ? current * 2.0f - quadControl : current;
                next = base + Vector2(a[0], a[1]);
                QuadTo(quadControl, next);
                break;
            }
            default: // 'A'
                next = base + Vector2(a[5], a[6]);
                ArcTo(current, {a[0], a[1]}, a[2], a[3] != 0, a[4] != 0, next);
                break;
            }
            current = next;
            previous = upper;
        }
        if (malformed)
        {
            Warn("path data is malformed; it is drawn up to the error");
        }
    }

    /// Paints the path built since *firstVerb* as the style asks: the fill, then the stroke, sharing the path.
    void AddShapes(const SvgStyle &style, uint32 firstVerb, uint32 firstPoint, bool fillable)
    {
        const uint32 verbCount = static_cast<uint32>(_verbs.Count()) - firstVerb;
        const uint32 pointCount = static_cast<uint32>(_points.Count()) - firstPoint;
        bool drawsSomething = false;
        for (uint32 v = firstVerb; v < firstVerb + verbCount; ++v)
        {
            drawsSomething = drawsSomething || (_verbs[v] != IconPathVerb::MoveTo && _verbs[v] != IconPathVerb::Close);
        }
        IconShape shape;
        shape.FirstVerb = firstVerb;
        shape.VerbCount = verbCount;
        shape.FirstPoint = firstPoint;
        shape.PointCount = pointCount;
        const auto setColor = [&](const Paint &paint, float32 opacity) {
            shape.CurrentColor = paint.Kind == PaintKind::CurrentColor;
            shape.Color = shape.CurrentColor ? LinearColor(1, 1, 1, opacity) : paint.Color;
            shape.Color.A = shape.CurrentColor ? opacity : paint.Color.A * opacity;
        };
        const usize shapesBefore = _shapes.Count();
        if (drawsSomething && style.Visible)
        {
            if (fillable && style.Fill.Kind != PaintKind::None)
            {
                shape.Kind = IconShapeKind::Fill;
                shape.FillRule = style.FillRule;
                setColor(style.Fill, style.Opacity * style.FillOpacity);
                _shapes.Add(shape);
            }
            if (style.Stroke.Kind != PaintKind::None && style.StrokeWidth > 0)
            {
                const UITransform &t = style.Transform;
                const float32 scaleX = Math::Sqrt(t.M11 * t.M11 + t.M12 * t.M12);
                const float32 scaleY = Math::Sqrt(t.M21 * t.M21 + t.M22 * t.M22);
                const float32 skew = t.M11 * t.M21 + t.M12 * t.M22;
                if (Math::Abs(scaleX - scaleY) > 1e-3f * Max(scaleX, scaleY) ||
                    Math::Abs(skew) > 1e-3f * scaleX * scaleY)
                {
                    Warn("strokes under non-uniform transforms keep a uniform width");
                }
                shape.Kind = IconShapeKind::Stroke;
                shape.StrokeWidth = style.StrokeWidth * Math::Sqrt(Math::Abs(t.GetDeterminant()));
                shape.Cap = style.Cap;
                shape.Join = style.Join;
                shape.MiterLimit = style.MiterLimit;
                setColor(style.Stroke, style.Opacity * style.StrokeOpacity);
                _shapes.Add(shape);
            }
        }
        if (_shapes.Count() == shapesBefore)
        {
            // Nothing paints the path: drop it.
            _verbs.Resize(firstVerb);
            _points.Resize(firstPoint);
        }
    }

    Icon &_icon;
    SvgIconDiagnostics &_diagnostics;
    List<Frame> _frames;
    List<IconShape> _shapes;
    List<IconPathVerb> _verbs;
    List<Vector2> _points;
    UITransform _transform;
};
} // namespace

bool ParseSvgIcon(StringView text, Icon &icon, SvgIconDiagnostics &diagnostics)
{
    diagnostics.Error = String();
    diagnostics.Warnings.Clear();
    icon.Size = {};
    SvgParser parser(icon, diagnostics);
    if (!parser.Parse(text))
    {
        if (diagnostics.Error.IsEmpty())
        {
            diagnostics.Error = "the file is not an SVG document";
        }
        return false;
    }
    return true;
}
} // namespace Marlin
