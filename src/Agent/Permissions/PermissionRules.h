#pragma once
#include <iiMultimodalAgent/agent/Types.h>

namespace iiMultimodalAgent::agent {
// Split comma/whitespace lists outside argument parentheses and validate rules.
// Throws on malformed input; at most 256 rules / 64 KiB per list.
IIMULTIMODALAGENT_EXPORT QStringList parsePermissionRules(const QStringList&);
}
