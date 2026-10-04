#include "GbmScanoutBuffer.h"

#include "drm_include.h"
#include "log.hpp"

#include <cinttypes>
#include <sys/stat.h>
#include <unistd.h>

#include <gbm.h>

namespace gamescope
{
    static LogScope s_GbmLog( "gbm" );

    class CGbmScanoutBuffer final : public IBackendScanoutBuffer
    {
    public:
        CGbmScanoutBuffer( std::shared_ptr<gbm_device> pDevice, gbm_bo *pBo, const wlr_dmabuf_attributes &attrs )
            : m_pDevice{ std::move( pDevice ) }
            , m_pBo{ pBo }
            , m_Attrs{ attrs }
        {
        }

        ~CGbmScanoutBuffer()
        {
            wlr_dmabuf_attributes_finish( &m_Attrs );
            gbm_bo_destroy( m_pBo );
        }

        const wlr_dmabuf_attributes *GetDmabufAttributes() const override { return &m_Attrs; }

    private:
        // Declared before m_pBo so the device outlives the bo.
        std::shared_ptr<gbm_device> m_pDevice;
        gbm_bo *m_pBo = nullptr;
        wlr_dmabuf_attributes m_Attrs = {};
    };

    std::shared_ptr<gbm_device> CreateGbmDevice( int nFd )
    {
        int nGbmFd = dup( nFd );
        if ( nGbmFd < 0 )
        {
            s_GbmLog.errorf_errno( "Failed to dup fd for GBM" );
            return nullptr;
        }

        gbm_device *pDevice = gbm_create_device( nGbmFd );
        if ( !pDevice )
        {
            close( nGbmFd );
            s_GbmLog.errorf( "Failed to create GBM device" );
            return nullptr;
        }

        s_GbmLog.infof( "Created GBM device (backend: %s)", gbm_device_get_backend_name( pDevice ) );
        return std::shared_ptr<gbm_device>( pDevice, [ nGbmFd ]( gbm_device *pDevice ){ gbm_device_destroy( pDevice ); close( nGbmFd ); } );
    }

    std::shared_ptr<IBackendScanoutBuffer> CreateGbmScanoutBuffer( const std::shared_ptr<gbm_device> &pDevice, uint32_t uWidth, uint32_t uHeight, uint32_t uDrmFormat, std::span<const uint64_t> ulModifiers, bool bLinear )
    {
        if ( !pDevice )
            return nullptr;

        uint32_t uFlags = GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT;

        bool bForceLinearModifier = false;
        struct gbm_bo *pBo = nullptr;
        if ( bLinear )
        {
            // GBM rejects GBM_BO_USE_LINEAR combined with an explicit
            // modifier list (EINVAL in Mesa), so allocate directly.
            pBo = gbm_bo_create( pDevice.get(), uWidth, uHeight, uDrmFormat, uFlags | GBM_BO_USE_LINEAR );
            bForceLinearModifier = true;
        }
        else if ( !ulModifiers.empty() )
        {
            pBo = gbm_bo_create_with_modifiers2( pDevice.get(), uWidth, uHeight, uDrmFormat, ulModifiers.data(), ulModifiers.size(), uFlags );

            // Some GBM implementations cannot allocate with an explicit
            // modifier list at all. If only LINEAR was requested, a plain
            // allocation with GBM_BO_USE_LINEAR is equivalent.
            if ( !pBo && ulModifiers.size() == 1 && ulModifiers[0] == DRM_FORMAT_MOD_LINEAR )
            {
                pBo = gbm_bo_create( pDevice.get(), uWidth, uHeight, uDrmFormat, uFlags | GBM_BO_USE_LINEAR );
                bForceLinearModifier = true;
            }
        }

        if ( !pBo )
        {
            s_GbmLog.errorf( "GBM allocation failed for %ux%u format 0x%x", uWidth, uHeight, uDrmFormat );
            return nullptr;
        }

        wlr_dmabuf_attributes attrs =
        {
            .width = int32_t( uWidth ),
            .height = int32_t( uHeight ),
            .format = uDrmFormat,
            .modifier = bForceLinearModifier ? DRM_FORMAT_MOD_LINEAR : gbm_bo_get_modifier( pBo ),
        };

        const int nPlaneCount = gbm_bo_get_plane_count( pBo );
        bool bSuccess = nPlaneCount >= 1 && nPlaneCount <= WLR_DMABUF_MAX_PLANES;

        if ( bSuccess && !bForceLinearModifier && !Algorithm::Contains( ulModifiers, attrs.modifier ) )
        {
            s_GbmLog.errorf( "GBM returned modifier 0x%" PRIx64 " which was not in the allowed list", attrs.modifier );
            bSuccess = false;
        }

        // Only count a plane in n_planes once we own its fd, so that
        // wlr_dmabuf_attributes_finish never closes an fd we didn't acquire.
        for ( int i = 0; bSuccess && i < nPlaneCount; i++ )
        {
            int nFd = gbm_bo_get_fd_for_plane( pBo, i );
            if ( nFd < 0 )
            {
                s_GbmLog.errorf_errno( "gbm_bo_get_fd_for_plane failed" );
                bSuccess = false;
                break;
            }
            attrs.fd[i] = nFd;
            attrs.offset[i] = gbm_bo_get_offset( pBo, i );
            attrs.stride[i] = gbm_bo_get_stride_for_plane( pBo, i );
            attrs.n_planes = i + 1;
        }
        bSuccess = bSuccess && attrs.n_planes == nPlaneCount;

        // Vulkan's dmabuf memory import is single-fd: all planes must
        // reference the same underlying buffer.
        for ( int i = 1; bSuccess && i < attrs.n_planes; i++ )
        {
            struct stat first, other;
            if ( fstat( attrs.fd[0], &first ) != 0 || fstat( attrs.fd[i], &other ) != 0 ||
                 first.st_dev != other.st_dev || first.st_ino != other.st_ino )
            {
                s_GbmLog.errorf( "GBM returned a disjoint multi-plane bo, cannot import" );
                bSuccess = false;
            }
        }

        if ( !bSuccess )
        {
            wlr_dmabuf_attributes_finish( &attrs );
            gbm_bo_destroy( pBo );
            return nullptr;
        }

        s_GbmLog.debugf( "allocated %ux%u format 0x%x modifier 0x%" PRIx64 " (%d planes)",
            uWidth, uHeight, uDrmFormat, attrs.modifier, attrs.n_planes );

        return std::make_shared<CGbmScanoutBuffer>( pDevice, pBo, attrs );
    }
}
