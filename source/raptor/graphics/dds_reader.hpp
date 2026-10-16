#pragma once

#include "foundation/platform.hpp"
#include "external/volk.h"

namespace raptor {

    bool dds_info( cstring path, int& width, int& height, u32& mip_levels, u32& size, VkFormat& format );
    u64  dds_mip_size( u32 width, u32 height ); // BC7: 16 bytes per 4x4 block.
    u8*  dds_load( cstring path, u32 size );
    void dds_free( u8* data );

} // namespace raptor
