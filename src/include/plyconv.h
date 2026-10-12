/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PLYCONV_H
#define PLYCONV_H

#include <cstdint>
#include <string>

#include "3d.h"
#include "ddb_export.h"
#include "fs.h"
#include "ply.h"

namespace ddb
{

    struct PlyToObjOptions
    {
        // When the header carries `comment TextureFile <name>`, write a minimal MTL
        // (`newmtl`, `map_Kd`), reference it with `mtllib`/`usemtl` and copy the
        // texture next to the OBJ. When false the comment is ignored (texture
        // coordinates are still written as `vt`).
        bool withTexture = true;
        // Root folder of the dataset that contains the PLY, or empty when unknown.
        // When set, a TextureFile that resolves outside it is rejected (it can never
        // be satisfied). Absolute TextureFile names are always rejected.
        std::string datasetRoot;
    };

    struct PlyToObjResult
    {
        std::string objPath;     // the OBJ written
        std::string mtlPath;     // the MTL written, empty when no texture was written
        std::string texturePath; // the texture copied next to the OBJ, empty when none
        uint64_t vertexCount = 0;   // `v` lines written (= PLY vertex count, no de-duplication)
        uint64_t texcoordCount = 0; // `vt` lines written
        bool hasColors = false;     // `v x y z r g b` written
        // Bounds (local model frame) and triangle count, in the form consumed by
        // computeObj2TilesOpts(). info.faceCount counts triangles after fan
        // triangulation; info.hasBounds is false only when there are no vertices.
        ModelInfo info;
    };

    /**
     * @brief Stream a mesh PLY into a Wavefront OBJ in constant memory.
     *
     * Supported input (anything else throws AppException with the element/index):
     *  - `format ascii|binary_little_endian|binary_big_endian 1.0`, LF or CRLF header;
     *  - every scalar type and alias (char/uchar/short/ushort/int/uint/float/double,
     *    int8...float64) and list properties with any count/item type;
     *  - the `vertex` element must precede the `face` element; elements before the
     *    face element are skipped, elements after it are not read;
     *  - faces from the list `vertex_indices` or `vertex_index`; triangles are kept,
     *    quads and n-gons are fan-triangulated (c0,ci,ci+1). Fan triangulation is
     *    wrong for concave polygons (known limitation). Faces with fewer than three
     *    corners are skipped;
     *  - vertex colours `red/green/blue`, `diffuse_red/green/blue` or `r/g/b`
     *    (uchar / ushort scaled to 0..1, float/double as-is, alpha ignored), written
     *    as `v x y z r g b` only when the vertex element has all three; the first
     *    complete set of a supported type is used. When the only complete sets have
     *    another type (e.g. `int red`), AppException names the property and type;
     *  - texture coordinates: per-face `texcoord` lists (one `vt` per corner, the
     *    list must hold 2 x corner values) or vertex `s,t` / `u,v` / `texture_u,
     *    texture_v` / `texture_s,texture_t` (one `vt` per vertex). Per-face wins
     *    when both exist. With texture coordinates every `f` corner is `v/vt`.
     * Numbers are written locale-independently with round-trip precision
     * (`%.9g` for float sources, `%.17g` otherwise). Vertices are not de-duplicated.
     * Every emitted position, colour and texture coordinate must be finite;
     * finite colours/UVs outside 0..1 are preserved. Degenerate face records are
     * fully consumed before being skipped, including any unused texcoord list.
     *
     * Multi-texture meshes (several `comment TextureFile` lines or a per-face
     * `texnumber` property) are rejected with AppException when withTexture is set
     * (identifyPly() classifies them as Generic).
     *
     * The texture check (see @ref getPlyTexture) runs on the header before any body
     * I/O. Partial generated outputs are removed on failure. Material and copied
     * texture filenames replace whitespace with underscores and are adjusted to
     * avoid collisions. The OBJ references the actual material filename, and
     * source textures are preserved; an OBJ output that aliases its source texture
     * is rejected before any output is opened.
     *
     * @param inputPly Path to the PLY.
     * @param outputBase Output path without extension (`<outputBase>.obj`);
     *                   a trailing `.obj` extension is accepted and kept. The OBJ
     *                   path is preserved; use result.mtlPath/result.texturePath
     *                   for the generated dependency filenames.
     * @throws FSException if the PLY cannot be opened or an output cannot be written.
     * @throws BuildDepMissingException if the TextureFile is missing (withTexture).
     * @throws AppException on unsupported or malformed input, or an unsatisfiable
     *         (absolute / root-escaping) TextureFile.
     */
    DDB_DLL PlyToObjResult convertPlyToObj(const std::string &inputPly, const std::string &outputBase,
                                           const PlyToObjOptions &opts = PlyToObjOptions());

    /**
     * @brief Header-only texture dependency check for a PLY.
     *
     * Reads only the header. When it has no `comment TextureFile`, returns an empty
     * string. Otherwise the name is normalised (`\` -> `/`, lexically normal) and
     * resolved against the PLY's folder.
     *
     * @return The resolved path of the existing texture, or empty when none is declared.
     * @throws AppException if the header is invalid, declares a multi-texture mesh
     *         (several TextureFile comments or a face `texnumber`), the name is
     *         absolute, or it resolves outside @p datasetRoot (when non-empty).
     * @throws BuildDepMissingException if the texture does not exist; the missing
     *         name is relative to the PLY's folder (normalised).
     */
    DDB_DLL std::string getPlyTexture(const std::string &inputPly, const std::string &datasetRoot = "");

}

#endif // PLYCONV_H
