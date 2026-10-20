/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CAD_H
#define CAD_H

#include <optional>
#include <string>
#include <vector>
#include "ddb_export.h"
#include "fs.h"
#include "gdal_inc.h"

// CAD drawings (DXF, DWG) as vector sources.
//
// GDAL reads DXF but not DWG, and ignores the coordinate system a drawing can
// carry. This module (1) turns a DWG into a temporary DXF with LibreDWG's
// dwg2dxf, run as a separate process (GPL-3.0 tool, never linked; a crash or
// hang in its decoder cannot take the host down), and (2) decides the CRS of a
// drawing, shared by indexing (footprint) and building (GPKG/MVT) so both agree.
namespace ddb
{
    namespace cad
    {
        // .dxf or .dwg (case-insensitive).
        DDB_DLL bool isCadFile(const fs::path &p);
        DDB_DLL bool isDwgFile(const fs::path &p);

        // Locates dwg2dxf: DDB_DWG2DXF_PATH (authoritative when set), next to the
        // running executable, <exe>/../bin, then PATH. Empty when unavailable.
        DDB_DLL fs::path findDwg2DxfBinary();

        // "AC1032 (R2018)" from the DWG header; throws AppException ("... is not a
        // valid DWG file") when the file does not start with a DWG version magic.
        DDB_DLL std::string dwgVersion(const fs::path &dwg);

        // Converts a DWG into a DXF. Implementations throw AppException with a
        // message a user can act on; outDxf exists only on success.
        class ICadConverter
        {
          public:
            virtual ~ICadConverter() = default;
            virtual void toDxf(const fs::path &dwg, const fs::path &outDxf) const = 0;
        };

        // LibreDWG dwg2dxf. The child is bounded by an idle timeout (60 s) and a
        // total run time (DDB_DWG2DXF_MAX_SECONDS, default 120 s). Exit code 0 is
        // not trusted: the output must open in GDAL and contain features.
        class DDB_DLL LibreDwgConverter : public ICadConverter
        {
          public:
            // Empty binary: findDwg2DxfBinary() at conversion time.
            explicit LibreDwgConverter(fs::path binary = fs::path());
            void toDxf(const fs::path &dwg, const fs::path &outDxf) const override;

          private:
            fs::path binary;
        };

        // A GDAL-openable DXF for a CAD file: the file itself for .dxf, a
        // temporary conversion for .dwg (deleted with this object).
        class DDB_DLL CadSource
        {
          public:
            // converter: nullptr = LibreDwgConverter. workDir: parent of the
            // temporary folder (empty = system temp directory).
            explicit CadSource(const fs::path &input, const ICadConverter *converter = nullptr,
                               const fs::path &workDir = fs::path());
            ~CadSource();
            CadSource(const CadSource &) = delete;
            CadSource &operator=(const CadSource &) = delete;

            const fs::path &dxfPath() const { return dxf; }
            bool converted() const { return !tempDir.empty(); }

          private:
            fs::path dxf;
            fs::path tempDir;
        };

        // Metadata GDAL does not expose, read from an ASCII DXF.
        struct DxfInfo
        {
            int insUnits = -1;          // $INSUNITS (-1: absent)
            int geoDataType = -1;       // GEODATA coordinate type (-1: no GEODATA)
            std::string geoDataDefinition; // GEODATA coordinate system definition (XML)
            int geoDataVersion = 0;     // GEODATA object version (code 90)
            // A local grid (type 1) places the drawing with these: the design point
            // (drawing coordinates) is the reference point (coordinate system), the
            // north direction is a drawing vector, the unit scale turns drawing units
            // into metres.
            double geoDesignX = 0, geoDesignY = 0;
            double geoReferenceX = 0, geoReferenceY = 0;
            double geoNorthX = 0, geoNorthY = 1;
            double geoUnitScale = 1;
        };
        DDB_DLL DxfInfo readDxfInfo(const fs::path &dxf);

        // Unit name for $INSUNITS ("m", "ft", "mm", ...; "" when unknown/unitless).
        DDB_DLL std::string insUnitsName(int insUnits);

        // SRS (authority code such as "EPSG:2193", or WKT) from a GEODATA object of
        // type 2 (projected grid) or 3 (geographic); type 0 (unset) and 1 (local grid,
        // see the DxfInfo overload) have none.
        DDB_DLL std::optional<std::string> srsFromGeoData(int type, const std::string &definition);

        // SRS of a drawing's GEODATA: types 2 and 3 as above; a local grid (type 1) of
        // a projected coordinate system becomes a WKT2 derived projected CRS whose
        // affine conversion takes the drawing's design point to the reference point,
        // turns the drawing's north direction to grid north and scales drawing units
        // to the system's units.
        DDB_DLL std::optional<std::string> srsFromGeoData(const DxfInfo &info);

        struct CadCrs
        {
            bool georeferenced = false;
            // Source SRS to assign to layers without one; empty when the layers
            // carry their own SRS or the drawing is local.
            std::string srs;
            // layer | prj | geodata | lonlat | none
            std::string source = "none";
            // Short display form: "EPSG:2193", a CRS name, or "".
            std::string label;
            // Extent of all layers in drawing coordinates (valid when hasExtent).
            OGREnvelope extent;
            bool hasExtent = false;
        };

        // Decides the CRS of a CAD drawing, first match wins:
        //   1. a layer SRS reported by GDAL;
        //   2. a same-name .prj next to originalInput (optional, the ArcGIS
        //      convention for CAD files); an unparsable .prj throws AppException,
        //      and a local (engineering) CRS such as LOCAL_CS["Drawing"] makes the
        //      drawing local on purpose;
        //   3. the drawing's GEODATA object;
        //   4. a DXF (never a DWG) without CRS whose extent fits in +-180/+-90:
        //      EPSG:4326, source "lonlat" (DDB-12 D1);
        //   5. otherwise local (not georeferenced: shown without a globe).
        DDB_DLL CadCrs resolveCadCrs(const fs::path &originalInput, GDALDatasetH ds, const DxfInfo &info);

        // The same-name .prj of a file, if one exists ("drawing.prj" or "drawing.PRJ")
        // and is not ignored by an IgnoredSidecarScope on this thread.
        DDB_DLL std::optional<fs::path> findPrjSidecar(const fs::path &file);

        // While alive, findPrjSidecar() ignores these files on the current thread.
        // The index re-reads a drawing right after its .prj was removed from the
        // index, and hosts delete the file only afterwards (Registry does), so the
        // removed .prj must not count even though it is still on disk.
        class DDB_DLL IgnoredSidecarScope
        {
          public:
            explicit IgnoredSidecarScope(std::vector<fs::path> files);
            ~IgnoredSidecarScope();
            IgnoredSidecarScope(const IgnoredSidecarScope &) = delete;
            IgnoredSidecarScope &operator=(const IgnoredSidecarScope &) = delete;

          private:
            std::vector<fs::path> previous;
        };
    }
}

#endif // CAD_H
