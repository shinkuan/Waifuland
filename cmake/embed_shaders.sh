#!/usr/bin/env bash
# Generate a C++ header embedding shader files as string literals.
# Usage: embed_shaders.sh <framework_shader_dir> <sample_shader_dir> <output_file>
set -euo pipefail

FRAMEWORK_DIR="$1"
SAMPLE_DIR="$2"
OUTPUT_FILE="$3"

mkdir -p "$(dirname "$OUTPUT_FILE")"

{
cat <<'HEADER'
// Auto-generated - do not edit
#pragma once

#include <cstddef>
#include <cstring>
#include <map>
#include <string>

namespace EmbeddedShaders {

HEADER

INDEX=0
ENTRIES=""

for PREFIX_DIR in "$FRAMEWORK_DIR:FrameworkShaders" "$SAMPLE_DIR:SampleShaders"; do
    DIR="${PREFIX_DIR%%:*}"
    PREFIX="${PREFIX_DIR##*:}"
    for SHADER in "$DIR"/*.frag "$DIR"/*.vert; do
        [ -f "$SHADER" ] || continue
        FNAME="$(basename "$SHADER")"
        KEY="${PREFIX}/${FNAME}"

        echo "static const char shader_${INDEX}_data[] ="
        echo "R\"SHADER($(cat "$SHADER"))SHADER\";"
        echo ""

        ENTRIES="${ENTRIES}        {\"${KEY}\", {shader_${INDEX}_data, sizeof(shader_${INDEX}_data) - 1}},
"
        INDEX=$((INDEX + 1))
    done
done

cat <<'STRUCT'
struct ShaderData {
    const char* data;
    size_t size;
};

STRUCT

echo "inline const std::map<std::string, ShaderData>& GetShaderMap() {"
echo "    static const std::map<std::string, ShaderData> shaders = {"
printf '%s' "$ENTRIES"
cat <<'FOOTER'
    };
    return shaders;
}

inline const ShaderData* Find(const std::string& path) {
    const auto& m = GetShaderMap();
    auto it = m.find(path);
    if (it != m.end()) return &it->second;
    return nullptr;
}

} // namespace EmbeddedShaders
FOOTER
} > "$OUTPUT_FILE"
