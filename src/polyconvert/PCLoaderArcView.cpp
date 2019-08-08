/****************************************************************************/
// Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.org/sumo
// Copyright (C) 2001-2019 German Aerospace Center (DLR) and others.
// This program and the accompanying materials
// are made available under the terms of the Eclipse Public License v2.0
// which accompanies this distribution, and is available at
// http://www.eclipse.org/legal/epl-v20.html
// SPDX-License-Identifier: EPL-2.0
/****************************************************************************/
/// @file    PCLoaderArcView.cpp
/// @author  Daniel Krajzewicz
/// @author  Jakob Erdmann
/// @author  Michael Behrisch
/// @date    Sept 2002
/// @version $Id$
///
// A reader of pois and polygons from shape files
/****************************************************************************/


// ===========================================================================
// included modules
// ===========================================================================
#include <config.h>

#include <string>
#include <utils/common/MsgHandler.h>
#include <utils/common/ToString.h>
#include <utils/common/StringUtils.h>
#include <utils/options/OptionsCont.h>
#include <utils/geom/GeomHelper.h>
#include <utils/geom/GeoConvHelper.h>
#include <utils/common/RGBColor.h>
#include <polyconvert/PCPolyContainer.h>
#include <polyconvert/PCTypeMap.h>
#include "PCLoaderArcView.h"

#ifdef HAVE_GDAL
#if __GNUC__ > 3
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <ogrsf_frmts.h>
#if __GNUC__ > 3
#pragma GCC diagnostic pop
#endif
#endif


// ===========================================================================
// method definitions
// ===========================================================================
void
PCLoaderArcView::loadIfSet(OptionsCont& oc, PCPolyContainer& toFill,
                           PCTypeMap& tm) {
    if (!oc.isSet("shapefile-prefixes")) {
        return;
    }
    // parse file(s)
    std::vector<std::string> files = oc.getStringVector("shapefile-prefixes");
    for (std::vector<std::string>::const_iterator file = files.begin(); file != files.end(); ++file) {
        PROGRESS_BEGIN_MESSAGE("Parsing from shape-file '" + *file + "'");
        load(*file, oc, toFill, tm);
        PROGRESS_DONE_MESSAGE();
    }
}



void
PCLoaderArcView::load(const std::string& file, OptionsCont& oc, PCPolyContainer& toFill,
                      PCTypeMap& tm) {
#ifdef HAVE_GDAL
    GeoConvHelper& geoConvHelper = GeoConvHelper::getProcessing();
    // get defaults
    const std::string idField = oc.getString("shapefile.id-column");
    const bool useRunningID = oc.getBool("shapefile.use-running-id") || idField == "";
    // start parsing
    std::string shpName = file + ".shp";
    int fillType = -1;
    if (oc.getString("shapefile.fill") == "true") {
        fillType = 1;
    } else if (oc.getString("shapefile.fill") == "false") {
        fillType = 0;
    }
#if GDAL_VERSION_MAJOR < 2
    OGRRegisterAll();
    OGRDataSource* poDS = OGRSFDriverRegistrar::Open(shpName.c_str(), FALSE);
#else
    GDALAllRegister();
    GDALDataset* poDS = (GDALDataset*) GDALOpenEx(shpName.c_str(), GDAL_OF_VECTOR | GA_ReadOnly, NULL, NULL, NULL);
#endif
    if (poDS == NULL) {
        throw ProcessError("Could not open shape description '" + shpName + "'.");
    }

    // begin file parsing
    OGRLayer*  poLayer = poDS->GetLayer(0);
    poLayer->ResetReading();

    // build coordinate transformation
    OGRSpatialReference* origTransf = poLayer->GetSpatialRef();
    OGRSpatialReference destTransf;
    // use wgs84 as destination
    destTransf.SetWellKnownGeogCS("WGS84");
    OGRCoordinateTransformation* poCT = OGRCreateCoordinateTransformation(origTransf, &destTransf);
    if (poCT == NULL) {
        if (oc.isSet("shapefile.guess-projection")) {
            OGRSpatialReference origTransf2;
            origTransf2.SetWellKnownGeogCS("WGS84");
            poCT = OGRCreateCoordinateTransformation(&origTransf2, &destTransf);
        }
        if (poCT == 0) {
            WRITE_WARNING("Could not create geocoordinates converter; check whether proj.4 is installed.");
        }
    }

    OGRFeature* poFeature;
    poLayer->ResetReading();
    int runningID = 0;
    while ((poFeature = poLayer->GetNextFeature()) != NULL) {
        std::vector<Parameterised*> parCont;
        // read in edge attributes
        std::string id = useRunningID ? toString(runningID) : poFeature->GetFieldAsString(idField.c_str());
        ++runningID;
        id = StringUtils::latin1_to_utf8(StringUtils::prune(id));
        if (id == "") {
            throw ProcessError("Missing id under '" + idField + "'");
        }
        id = oc.getString("prefix") + id;
        std::string type;
        for (const std::string& typeField : oc.getStringVector("shapefile.type-columns")) {
            if (type != "") {
                type += ".";
            }
            type += poFeature->GetFieldAsString(typeField.c_str());
        }
        RGBColor color = RGBColor::parseColor(oc.getString("color"));
        double layer = oc.getFloat("layer");
        double angle = Shape::DEFAULT_ANGLE;
        std::string imgFile = Shape::DEFAULT_IMG_FILE;
        if (type != "" && tm.has(type)) {
            const PCTypeMap::TypeDef& def = tm.get(type);
            if (def.discard) {
                continue;
            }
            color = def.color;
            layer = def.layer;
            angle = def.angle;
            imgFile = def.imgFile;
            type = def.id;
        } else {
            type = oc.getString("type");
        }
        if (poFeature->GetFieldIndex("angle") >= 0) {
            angle = poFeature->GetFieldAsDouble("angle");
        }
        // read in the geometry
        OGRGeometry* poGeometry = poFeature->GetGeometryRef();
        if (poGeometry == 0) {
            OGRFeature::DestroyFeature(poFeature);
            continue;
        }
        // try transform to wgs84
        poGeometry->transform(poCT);
        OGRwkbGeometryType gtype = poGeometry->getGeometryType();
        switch (gtype) {
            case wkbPoint: {
                OGRPoint* cgeom = (OGRPoint*) poGeometry;
                Position pos(cgeom->getX(), cgeom->getY());
                if (!geoConvHelper.x2cartesian(pos)) {
                    WRITE_ERROR("Unable to project coordinates for POI '" + id + "'.");
                }
                PointOfInterest* poi = new PointOfInterest(id, type, color, pos, false, "", 0, 0, layer, angle, imgFile);
                if (toFill.add(poi)) {
                    parCont.push_back(poi);
                }
            }
            break;
            case wkbLineString: {
                bool fill = fillType < 0 ? false : (fillType == 1);
                OGRLineString* cgeom = (OGRLineString*) poGeometry;
                PositionVector shape;
                for (int j = 0; j < cgeom->getNumPoints(); j++) {
                    Position pos((double) cgeom->getX(j), (double) cgeom->getY(j));
                    if (!geoConvHelper.x2cartesian(pos)) {
                        WRITE_ERROR("Unable to project coordinates for polygon '" + id + "'.");
                    }
                    shape.push_back_noDoublePos(pos);
                }
                SUMOPolygon* poly = new SUMOPolygon(id, type, color, shape, false, fill, 1, layer, angle, imgFile);
                if (toFill.add(poly)) {
                    parCont.push_back(poly);
                }
            }
            break;
            case wkbPolygon: {
                bool fill = fillType < 0 ? true : (fillType == 1);
                OGRLinearRing* cgeom = ((OGRPolygon*) poGeometry)->getExteriorRing();
                PositionVector shape;
                for (int j = 0; j < cgeom->getNumPoints(); j++) {
                    Position pos((double) cgeom->getX(j), (double) cgeom->getY(j));
                    if (!geoConvHelper.x2cartesian(pos)) {
                        WRITE_ERROR("Unable to project coordinates for polygon '" + id + "'.");
                    }
                    shape.push_back_noDoublePos(pos);
                }
                SUMOPolygon* poly = new SUMOPolygon(id, type, color, shape, false, fill, 1, layer, angle, imgFile);
                if (toFill.add(poly)) {
                    parCont.push_back(poly);
                }
            }
            break;
            case wkbMultiPoint: {
                OGRMultiPoint* cgeom = (OGRMultiPoint*) poGeometry;
                for (int i = 0; i < cgeom->getNumGeometries(); ++i) {
                    OGRPoint* cgeom2 = (OGRPoint*) cgeom->getGeometryRef(i);
                    Position pos(cgeom2->getX(), cgeom2->getY());
                    std::string tid = id + "#" + toString(i);
                    if (!geoConvHelper.x2cartesian(pos)) {
                        WRITE_ERROR("Unable to project coordinates for POI '" + tid + "'.");
                    }
                    PointOfInterest* poi = new PointOfInterest(tid, type, color, pos, false, "", 0, 0, layer, angle, imgFile);
                    if (toFill.add(poi)) {
                        parCont.push_back(poi);
                    }
                }
            }
            break;
            case wkbMultiLineString: {
                bool fill = fillType < 0 ? false : (fillType == 1);
                OGRMultiLineString* cgeom = (OGRMultiLineString*) poGeometry;
                for (int i = 0; i < cgeom->getNumGeometries(); ++i) {
                    OGRLineString* cgeom2 = (OGRLineString*) cgeom->getGeometryRef(i);
                    PositionVector shape;
                    std::string tid = id + "#" + toString(i);
                    for (int j = 0; j < cgeom2->getNumPoints(); j++) {
                        Position pos(cgeom2->getX(j), cgeom2->getY(j));
                        if (!geoConvHelper.x2cartesian(pos)) {
                            WRITE_ERROR("Unable to project coordinates for polygon '" + tid + "'.");
                        }
                        shape.push_back_noDoublePos(pos);
                    }
                    SUMOPolygon* poly = new SUMOPolygon(tid, type, color, shape, false, fill, 1, layer, angle, imgFile);
                    if (toFill.add(poly)) {
                        parCont.push_back(poly);
                    }
                }
            }
            break;
            case wkbMultiPolygon: {
                bool fill = fillType < 0 ? true : (fillType == 1);
                OGRMultiPolygon* cgeom = (OGRMultiPolygon*) poGeometry;
                for (int i = 0; i < cgeom->getNumGeometries(); ++i) {
                    OGRLinearRing* cgeom2 = ((OGRPolygon*) cgeom->getGeometryRef(i))->getExteriorRing();
                    PositionVector shape;
                    std::string tid = id + "#" + toString(i);
                    for (int j = 0; j < cgeom2->getNumPoints(); j++) {
                        Position pos((double) cgeom2->getX(j), (double) cgeom2->getY(j));
                        if (!geoConvHelper.x2cartesian(pos)) {
                            WRITE_ERROR("Unable to project coordinates for polygon '" + tid + "'.");
                        }
                        shape.push_back_noDoublePos(pos);
                    }
                    SUMOPolygon* poly = new SUMOPolygon(tid, type, color, shape, false, fill, 1, layer, angle, imgFile);
                    if (toFill.add(poly)) {
                        parCont.push_back(poly);
                    }
                }
            }
            break;
            default:
                WRITE_WARNING("Unsupported shape type occurred (id='" + id + "').");
                break;
        }
        if (oc.getBool("shapefile.add-param")) {
            for (std::vector<Parameterised*>::const_iterator it = parCont.begin(); it != parCont.end(); ++it) {
                OGRFeatureDefn* poFDefn = poLayer->GetLayerDefn();
                for (int iField = 0; iField < poFDefn->GetFieldCount(); iField++) {
                    OGRFieldDefn* poFieldDefn = poFDefn->GetFieldDefn(iField);
                    if (poFieldDefn->GetNameRef() != idField) {
                        if (poFieldDefn->GetType() == OFTReal) {
                            (*it)->setParameter(poFieldDefn->GetNameRef(), toString(poFeature->GetFieldAsDouble(iField)));
                        } else {
                            (*it)->setParameter(poFieldDefn->GetNameRef(), StringUtils::latin1_to_utf8(poFeature->GetFieldAsString(iField)));
                        }
                    }
                }
            }
        }
        OGRFeature::DestroyFeature(poFeature);
    }
#if GDAL_VERSION_MAJOR < 2
    OGRDataSource::DestroyDataSource(poDS);
#else
    GDALClose(poDS);
#endif
    PROGRESS_DONE_MESSAGE();
#else
    UNUSED_PARAMETER(file);
    UNUSED_PARAMETER(oc);
    UNUSED_PARAMETER(toFill);
    WRITE_ERROR("SUMO was compiled without GDAL support.");
#endif
}


/****************************************************************************/

