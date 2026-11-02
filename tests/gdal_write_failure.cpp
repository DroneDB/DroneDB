/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_write_failure.h"

#include <cerrno>
#include <functional>

#include "gdal_inc.h"
#include "gtest/gtest.h"

namespace ddb_test {
namespace {
// Fault injection lives entirely in the GDAL test driver. Translations still
// copy real source features; the output factory/close supplies a structured OS
// write failure without relying on its human-readable message.
GDALDriver::CreateCallback originalVectorCreate = nullptr;
GDALDriver* failingVectorDriver = nullptr;
bool failVectorClose = false;

class CloseFailingVectorDataset : public GDALDataset {
public:
    explicit CloseFailingVectorDataset(GDALDataset* dataset) : underlying(dataset) {
        SetDescription(dataset->GetDescription());
        eAccess = GA_Update;
    }
    ~CloseFailingVectorDataset() override { if (underlying) GDALClose(underlying); }
#if GDAL_VERSION_NUM >= 3130000
    GDALDriver* GetDriver() const override { return failingVectorDriver; }
#else
    GDALDriver* GetDriver() override { return failingVectorDriver; }
#endif
#if GDAL_VERSION_NUM >= 3120000
    int GetLayerCount() const override { return underlying->GetLayerCount(); }
    const OGRLayer* GetLayer(int index) const override { return underlying->GetLayer(index); }
    int TestCapability(const char* capability) const override { return underlying->TestCapability(capability); }
#else
    int GetLayerCount() override { return underlying->GetLayerCount(); }
    OGRLayer* GetLayer(int index) override { return underlying->GetLayer(index); }
    int TestCapability(const char* capability) override { return underlying->TestCapability(capability); }
#endif
    OGRLayer* GetLayerByName(const char* name) override { return underlying->GetLayerByName(name); }
    OGRErr StartTransaction(int force) override { return underlying->StartTransaction(force); }
    OGRErr CommitTransaction() override { return underlying->CommitTransaction(); }
    OGRErr RollbackTransaction() override { return underlying->RollbackTransaction(); }
#if GDAL_VERSION_NUM >= 3070000
#if GDAL_VERSION_NUM >= 3130000
    CPLErr Close(GDALProgressFunc, void*) override {
#else
    CPLErr Close() override {
#endif
        if (!underlying) return CE_None;
        GDALClose(underlying);
        underlying = nullptr;
        GDALDataset::Close();
        CPLError(CE_Failure, CPLE_FileIO, "test storage rejected final flush");
        errno = ENOSPC;
        return CE_Failure;
    }
#endif
protected:
#if GDAL_VERSION_NUM >= 3120000
    OGRLayer* ICreateLayer(const char* name, const OGRGeomFieldDefn* definition,
                           CSLConstList options) override {
        return underlying->CreateLayer(name, definition, options);
    }
#else
    OGRLayer* ICreateLayer(const char* name, OGRSpatialReference* reference,
                           OGRwkbGeometryType geometry, char** options) override {
        return underlying->CreateLayer(name, reference, geometry, options);
    }
#endif
private:
    GDALDataset* underlying;
};

#if GDAL_VERSION_NUM >= 3130000
using CreateOptions = CSLConstList;
#else
using CreateOptions = char**;
#endif

GDALDataset* failingVectorCreate(const char* name, int width, int height, int bands,
                                 GDALDataType type, CreateOptions options) {
    if (!failVectorClose) {
        CPLError(CE_Failure, CPLE_FileIO, "test storage rejected output creation");
        errno = ENOSPC;
        return nullptr;
    }
    GDALDataset* dataset = originalVectorCreate(name, width, height, bands, type, options);
    return dataset ? new CloseFailingVectorDataset(dataset) : nullptr;
}
} // namespace

void withVectorWriteFailure(const char* driverName, bool onClose, const std::function<void()>& operation) {
    GDALAllRegister();
    auto* driver = GetGDALDriverManager()->GetDriverByName(driverName);
    ASSERT_NE(driver, nullptr);
    ASSERT_NE(driver->pfnCreate, nullptr);
    struct RestoreDriver {
        GDALDriver* driver;
        GDALDriver::CreateCallback original;
        ~RestoreDriver() {
            driver->pfnCreate = original;
            originalVectorCreate = nullptr;
            failingVectorDriver = nullptr;
        }
    } restore{driver, driver->pfnCreate};
    originalVectorCreate = driver->pfnCreate;
    failingVectorDriver = driver;
    failVectorClose = onClose;
    driver->pfnCreate = failingVectorCreate;
    operation();
}
} // namespace ddb_test
