#pragma once

#include "backend.h"

#include <cstdint>
#include <memory>
#include <span>

struct gbm_device;

namespace gamescope
{
    // Creates a GBM device on a dup of nFd. The device owns the dup, so buffers
    // that hold the device keep working after the caller closes nFd.
    std::shared_ptr<gbm_device> CreateGbmDevice( int nFd );

    // Allocates a scanout-capable buffer with GBM_BO_USE_SCANOUT. ulModifiers
    // must not contain DRM_FORMAT_MOD_INVALID; returns nullptr on failure.
    std::shared_ptr<IBackendScanoutBuffer> CreateGbmScanoutBuffer( const std::shared_ptr<gbm_device> &pDevice, uint32_t uWidth, uint32_t uHeight, uint32_t uDrmFormat, std::span<const uint64_t> ulModifiers, bool bLinear );
}
