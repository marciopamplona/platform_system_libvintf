/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VintfObject.h"

#include "CompatibilityMatrix.h"
#include "parse_xml.h"

#include <functional>
#include <memory>
#include <mutex>

namespace android {
namespace vintf {

template <typename T>
struct LockedUniquePtr {
    std::unique_ptr<T> object;
    std::mutex mutex;
};

static LockedUniquePtr<HalManifest> gDeviceManifest;
static LockedUniquePtr<HalManifest> gFrameworkManifest;
static LockedUniquePtr<RuntimeInfo> gDeviceRuntimeInfo;

template <typename T, typename F>
static const T *Get(
        LockedUniquePtr<T> *ptr,
        bool skipCache,
        const F &fetchAllInformation) {
    std::unique_lock<std::mutex> _lock(ptr->mutex);
    if (skipCache || ptr->object == nullptr) {
        ptr->object = std::make_unique<T>();
        if (fetchAllInformation(ptr->object.get()) != OK) {
            ptr->object = nullptr; // frees the old object
        }
    }
    return ptr->object.get();
}

// static
const HalManifest *VintfObject::GetDeviceHalManifest(bool skipCache) {
    return Get(&gDeviceManifest, skipCache,
            std::bind(&HalManifest::fetchAllInformation, std::placeholders::_1,
                "/vendor/manifest.xml"));
}

// static
const HalManifest *VintfObject::GetFrameworkHalManifest(bool skipCache) {
    return Get(&gFrameworkManifest, skipCache,
            std::bind(&HalManifest::fetchAllInformation, std::placeholders::_1,
                "/system/manifest.xml"));
}

// static
const RuntimeInfo *VintfObject::GetRuntimeInfo(bool skipCache) {
    return Get(&gDeviceRuntimeInfo, skipCache,
            std::bind(&RuntimeInfo::fetchAllInformation, std::placeholders::_1));
}

namespace details {

static status_t mountSystem() {
    // TODO(b/36814503): mount system partition here for recovery mode.
    return OK;
}

static status_t mountVendor() {
    // TODO(b/36814503): mount vendor partition here for recovery mode.
    return OK;
}

enum class ParseStatus {
    OK,
    PARSE_ERROR,
    DUPLICATED_FWK_ENTRY,
    DUPLICATED_DEV_ENTRY,
};

std::string toString(ParseStatus status) {
    switch(status) {
        case ParseStatus::OK:                   return "OK";
        case ParseStatus::PARSE_ERROR:          return "parse error";
        case ParseStatus::DUPLICATED_FWK_ENTRY: return "duplicated framework";
        case ParseStatus::DUPLICATED_DEV_ENTRY: return "duplicated device";
    }
    return "";
}

template<typename T>
ParseStatus tryParse(const std::string &xml, const XmlConverter<T> &parse,
        std::unique_ptr<T> *fwk, std::unique_ptr<T> *dev) {
    std::unique_ptr<T> ret = std::make_unique<T>();
    if (!parse(ret.get(), xml)) {
        return ParseStatus::PARSE_ERROR;
    }
    if (ret->type() == SchemaType::FRAMEWORK) {
        if (fwk->get() != nullptr) {
            return ParseStatus::DUPLICATED_FWK_ENTRY;
        }
        *fwk = std::move(ret);
    } else if (ret->type() == SchemaType::DEVICE) {
        if (dev->get() != nullptr) {
            return ParseStatus::DUPLICATED_DEV_ENTRY;
        }
        *dev = std::move(ret);
    }
    return ParseStatus::OK;
}

template<typename T, typename GetFunction>
static status_t getMissing(const T *pkg, bool mount,
        std::function<status_t(void)> mountFunction,
        const T **updated,
        GetFunction getFunction) {
    if (pkg != nullptr) {
        *updated = pkg;
    } else {
        if (mount) {
            (void)mountFunction(); // ignore mount errors
        }
        *updated = getFunction();
    }
    return OK;
}

#define ADD_MESSAGE(__error__)  \
    if (error != nullptr) {     \
        *error += (__error__);  \
    }                           \

struct PackageInfo {
    struct Pair {
        std::unique_ptr<HalManifest>         manifest;
        std::unique_ptr<CompatibilityMatrix> matrix;
    };
    Pair dev;
    Pair fwk;
};

struct UpdatedInfo {
    struct Pair {
        const HalManifest         *manifest;
        const CompatibilityMatrix *matrix;
    };
    Pair dev;
    Pair fwk;
    const RuntimeInfo *runtimeInfo;
};

// Parse all information from package;
// Get missing information from the device;
// Do compatibility check.
int32_t checkCompatibility(const std::vector<std::string> &xmls, bool mount,
        std::function<status_t(void)> mountSystem,
        std::function<status_t(void)> mountVendor,
        std::function<const HalManifest *(bool)> GetFrameworkHalManifest,
        std::function<const HalManifest *(bool)> GetDeviceHalManifest,
        std::function<const RuntimeInfo *(bool)> GetRuntimeInfo,
        std::string *error) {

    status_t status;
    ParseStatus parseStatus;
    PackageInfo pkg; // All information from package.
    UpdatedInfo updated; // All files and runtime info after the update.

    if (xmls.empty()) {
        ADD_MESSAGE("nothing to update");
        return BAD_VALUE;
    }

    // parse all information from package
    for (const auto &xml : xmls) {
        parseStatus = tryParse(xml, gHalManifestConverter, &pkg.fwk.manifest, &pkg.dev.manifest);
        if (parseStatus == ParseStatus::OK) {
            continue; // work on next one
        }
        if (parseStatus != ParseStatus::PARSE_ERROR) {
            ADD_MESSAGE(toString(parseStatus) + " manifest");
            return ALREADY_EXISTS;
        }
        parseStatus = tryParse(xml, gCompatibilityMatrixConverter, &pkg.fwk.matrix, &pkg.dev.matrix);
        if (parseStatus == ParseStatus::OK) {
            continue; // work on next one
        }
        if (parseStatus != ParseStatus::PARSE_ERROR) {
            ADD_MESSAGE(toString(parseStatus) + " matrix");
            return ALREADY_EXISTS;
        }
        ADD_MESSAGE(toString(parseStatus)); // parse error
        return BAD_VALUE;
    }

    // get missing info from device
    if ((status = getMissing(pkg.fwk.manifest.get(), mount, mountSystem, &updated.fwk.manifest,
            std::bind(GetFrameworkHalManifest, true /* skipCache */))) != OK) {
        return status;
    }
    if ((status = getMissing(pkg.dev.manifest.get(), mount, mountVendor, &updated.dev.manifest,
            std::bind(GetDeviceHalManifest, true /* skipCache */))) != OK) {
        return status;
    }
    updated.runtimeInfo = GetRuntimeInfo(true /* skipCache */);
    // TODO(b/37321309) get matrices from the device as well.
    updated.fwk.matrix = pkg.fwk.matrix.get();
    updated.dev.matrix = pkg.dev.matrix.get();

    // null checks for files and runtime info after the update
    // TODO(b/37321309) if a compat mat is missing, it is not matched and considered compatible.
    if (updated.fwk.manifest == nullptr) {
        ADD_MESSAGE("No framework manifest file from device or from update package");
        return NO_INIT;
    }
    if (updated.dev.manifest == nullptr) {
        ADD_MESSAGE("No device manifest file from device or from update package");
        return NO_INIT;
    }
    if (updated.fwk.matrix == nullptr) {
        ADD_MESSAGE("No framework matrix, skipping;");
        // TODO(b/37321309) consider missing matricies as errors.
    }
    if (updated.dev.matrix == nullptr) {
        ADD_MESSAGE("No device matrix, skipping;");
        // TODO(b/37321309) consider missing matricies as errors.
    }
    if (updated.runtimeInfo == nullptr) {
        ADD_MESSAGE("No runtime info from device");
        return NO_INIT;
    }

    // compatiblity check.
    // TODO(b/37321309) outer if checks can be removed if we consider missing matrices as errors.
    if (updated.dev.manifest && updated.fwk.matrix) {
        if (!updated.dev.manifest->checkCompatibility(*updated.fwk.matrix, error)) {
            error->insert(0, "Device manifest and framework compatibility matrix "
                             "are incompatible: ");
            return INCOMPATIBLE;
        }
    }
    if (updated.fwk.manifest && updated.dev.matrix) {
        if (!updated.fwk.manifest->checkCompatibility(*updated.dev.matrix, error)) {
            error->insert(0, "Framework manifest and device compatibility matrix "
                             "are incompatible: ");
            return INCOMPATIBLE;
        }
    }
    if (updated.runtimeInfo && updated.fwk.matrix) {
        if (!updated.runtimeInfo->checkCompatibility(*updated.fwk.matrix, error)) {
            error->insert(0, "Runtime info and framework compatibility matrix "
                             "are incompatible: ");
            return INCOMPATIBLE;
        }
    }

    return COMPATIBLE;
}

} // namespace details

// static
int32_t VintfObject::CheckCompatibility(
        const std::vector<std::string> &xmls,
        bool mount, std::string *error) {
    return details::checkCompatibility(xmls, mount,
            &details::mountSystem, &details::mountVendor,
            &VintfObject::GetFrameworkHalManifest,
            &VintfObject::GetDeviceHalManifest,
            &VintfObject::GetRuntimeInfo,
            error);
}


} // namespace vintf
} // namespace android
