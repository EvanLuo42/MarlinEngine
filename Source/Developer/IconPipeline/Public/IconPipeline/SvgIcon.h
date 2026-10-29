#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"
#include "UIIcon/Icon.h"

namespace Marlin
{
/// What ParseSvgIcon found wrong: an error when the file cannot become an icon, and one warning per kind of content
/// it skipped or approximated.
struct SvgIconDiagnostics
{
    String Error;
    List<String> Warnings;
};

/// Bakes SVG text into *icon*'s view box size and shapes. Reads the subset icon sets use: the root `svg` (viewBox,
/// or width and height), `g`, `path` (every command, absolute and relative; arcs become cubic curves), `circle`,
/// `ellipse`, `rect` (with rx and ry), `line`, `polyline` and `polygon`; the presentation attributes, also in
/// `style`: fill, stroke, stroke-width, stroke-linecap, stroke-linejoin, stroke-miterlimit, fill-rule, opacity,
/// fill-opacity, stroke-opacity, display and visibility; and transform (matrix, translate, scale, rotate, skewX,
/// skewY), baked into the points. Colors are currentColor, none, #rgb, #rgba, #rrggbb, #rrggbbaa, rgb() and the basic
/// named colors, converted from sRGB to linear.
///
/// Anything else, such as gradients, `use`, clip paths, masks, text or dashes, is skipped with a warning; malformed
/// path data draws up to the error, as browsers do. False, with Error set, if the text is not an SVG document.
[[nodiscard]] ICONPIPELINE_API bool ParseSvgIcon(StringView text, Icon &icon, SvgIconDiagnostics &diagnostics);
} // namespace Marlin
