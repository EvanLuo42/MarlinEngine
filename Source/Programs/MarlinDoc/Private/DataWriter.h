#pragma once

#include "Model.h"

namespace Marlin::Doc
{
/// The version of the data's format: raised only for changes that are not compatible (Documentation/Architecture/16
/// §4).
inline constexpr int64 DataFormatVersion = 1;

/// Writes `index.json`, `Modules/<Module>.json` and `Books/<Book>.json` into *folder*.
bool WriteData(const Project &project, StringView folder, Reporter &reporter);
} // namespace Marlin::Doc
