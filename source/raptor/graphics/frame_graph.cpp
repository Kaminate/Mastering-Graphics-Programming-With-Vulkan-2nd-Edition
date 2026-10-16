#include "frame_graph.hpp"

#include "foundation/file.hpp"
#include "foundation/memory.hpp"
#include "foundation/string.hpp"
#include "foundation/numerics.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"
#include "graphics/gpu_resources.hpp"
#include "graphics/frame_renderer.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/renderer.hpp"

#include "external/json.hpp"
#include "external/imgui/imgui.h"
#include "external/imgui-node-editor/imgui_node_editor.h"
#include "external/tracy/tracy/Tracy.hpp"

namespace ed = ax::NodeEditor;

#include <string>

#define FRAME_GRAPH_DEBUG 0

namespace raptor
{

static FrameGraphResourceType string_to_resource_type( cstring input_type ) {
    if ( strcmp( input_type, "texture" ) == 0 ) {
        return FrameGraphResourceType_Texture;
    }

    if ( strcmp( input_type, "attachment" ) == 0 ) {
        return FrameGraphResourceType_Attachment;
    }

    if ( strcmp( input_type, "buffer" ) == 0 ) {
        return FrameGraphResourceType_Buffer;
    }

    if ( strcmp( input_type, "reference" ) == 0 ) {
        // This is used for resources that need to create an edge but are not actually
        // used by the render pass
        return FrameGraphResourceType_Reference;
    }

    if ( strcmp( input_type, "shading_rate" ) == 0 ) {
        return FrameGraphResourceType_ShadingRate;
    }

    RASSERT( false );
    return FrameGraphResourceType_Invalid;
}

VkAttachmentLoadOp string_to_render_pass_operation( cstring op ) {
    if ( strcmp( op, "clear" ) == 0 ) {
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    } else if ( strcmp( op, "load" ) == 0 ) {
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    RASSERT( false );
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

// FrameGraph /////////////////////////////////////////////////////////////

void FrameGraph::init( FrameGraphBuilder* builder_ ) {
    allocator = &MemoryService::instance()->system_allocator;

    local_allocator.init( rmega( 1 ) );

    builder = builder_;

    nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count );
    all_nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count );
    persistent_update_nodes.init( allocator, 8 );
    batches.init( allocator, 8 );

    ed::Config config;
    config.SettingsFile = "framegraph.json";
    ed_context = ed::CreateEditor(&config);

    clear_debug_image_selection();
}

void FrameGraph::shutdown() {
    clear_debug_image_selection();

    ed::DestroyEditor( ed_context );
    ed_context = nullptr;

    for ( u32 i = 0; i < all_nodes.size; ++i ) {
        FrameGraphNodeHandle handle = all_nodes[ i ];
        FrameGraphNode* node = builder->access_node( handle );

        node->inputs.shutdown();
        node->temp_inputs_type.shutdown();
        node->outputs.shutdown();
        node->edges.shutdown();
    }

    for ( u32 i = 0; i < batches.size; i++ ) {
        FrameGraphExecutionBatch& batch = batches[ i ];
        batch.nodes.shutdown();
    }

    batches.shutdown();
    persistent_update_nodes.shutdown();
    all_nodes.shutdown();
    nodes.shutdown();
    node_layer.shutdown();

    local_allocator.shutdown();
}

void FrameGraph::parse( cstring file_path, ArenaAllocator* temp_allocator ) {
    using json = nlohmann::json;

    if ( !file_exists( file_path ) ) {
        rprint( "Cannot find file %s\n", file_path );
        return;
    }

    sizet current_allocator_marker = temp_allocator->get_marker();

    FileReadResult read_result = file_read_text( file_path, temp_allocator );

    json graph_data = json::parse( read_result.data );

    StringBuffer string_buffer;
    string_buffer.init( 2048, &local_allocator );

    std::string name_value = graph_data.value( "name", "" );
    name = string_buffer.append_use_f( "%s", name_value.c_str() );

    json passes = graph_data[ "passes" ];
    for ( sizet i = 0; i < passes.size(); ++i ) {
        json pass = passes[ i ];

        json pass_inputs = pass[ "inputs" ];
        json pass_outputs = pass[ "outputs" ];

        FrameGraphNodeCreation node_creation{ };
        Array<FrameGraphResourceCreation> node_inputs{ };
        node_inputs.init( temp_allocator, (u32)pass_inputs.size() );
        Array<FrameGraphResourceCreation> node_outputs{ };
        node_outputs.init( temp_allocator, (u32)pass_outputs.size() );

        std::string node_type = pass.value( "type", "" );
        node_creation.compute = node_type.compare( "compute" ) == 0;
        node_creation.ray_tracing = node_type.compare( "ray_tracing" ) == 0;

        for ( sizet ii = 0; ii < pass_inputs.size(); ++ii ) {
            json pass_input = pass_inputs[ ii ];

            FrameGraphResourceCreation input_creation{ };

            std::string input_type = pass_input.value( "type", "" );
            RASSERT( !input_type.empty() );

            std::string input_name = pass_input.value( "name", "" );
            RASSERT( !input_name.empty() );

            input_creation.type = string_to_resource_type( input_type.c_str() );
            input_creation.resource_info.external = false;

            input_creation.name = string_buffer.append_use_f( "%s", input_name.c_str() );

            node_inputs.push( input_creation );
        }

        for ( sizet oi = 0; oi < pass_outputs.size(); ++oi ) {
            json pass_output = pass_outputs[ oi ];

            FrameGraphResourceCreation output_creation{ };

            std::string output_type = pass_output.value( "type", "" );
            RASSERT( !output_type.empty() );

            std::string output_name = pass_output.value( "name", "" );
            RASSERT( !output_name.empty() );

            bool external = pass_output.value( "external", false );
            output_creation.resource_info.external = external;

            output_creation.type = string_to_resource_type( output_type.c_str() );
            output_creation.name = string_buffer.append_use_f( "%s", output_name.c_str() );

            switch ( output_creation.type ) {
                case FrameGraphResourceType_Texture:
                {
                    // NOTE(marco): for now output textures are all managed manually. We add them to the graph
                    // to make sure they are considered when performing the topological sort
                } break;
                case FrameGraphResourceType_Attachment:
                {
                    std::string format = pass_output.value( "format", "" );
                    RASSERT( !format.empty() );

                    output_creation.resource_info.texture.format = util_string_to_vk_format( format.c_str() );

                    std::string load_op = pass_output.value( "load_operation", "" );
                    RASSERT( !load_op.empty() );

                    output_creation.resource_info.texture.load_op = string_to_render_pass_operation( load_op.c_str() );

                    json resolution = pass_output[ "resolution" ];
                    json scaling = pass_output[ "resolution_scale" ];

                    if ( resolution.is_array() ) {
                        output_creation.resource_info.texture.width = resolution[ 0 ];
                        output_creation.resource_info.texture.height = resolution[ 1 ];
                        output_creation.resource_info.texture.depth = 1;
                        output_creation.resource_info.texture.scale_width = 0.f;
                        output_creation.resource_info.texture.scale_height = 0.f;
                    }
                    else if ( scaling.is_array() ) {
                        output_creation.resource_info.texture.width = 0;
                        output_creation.resource_info.texture.height = 0;
                        output_creation.resource_info.texture.depth = 1;
                        output_creation.resource_info.texture.scale_width = scaling[ 0 ];
                        output_creation.resource_info.texture.scale_height = scaling[ 1 ];
                    }
                    else {
                        // Defaults
                        output_creation.resource_info.texture.width = 0;
                        output_creation.resource_info.texture.height = 0;
                        output_creation.resource_info.texture.depth = 1;
                        output_creation.resource_info.texture.scale_width = 1.f;
                        output_creation.resource_info.texture.scale_height = 1.f;
                    }

                    output_creation.resource_info.texture.compute = node_creation.compute;

                    // Parse depth/stencil values
                    if ( TextureFormat::has_depth( output_creation.resource_info.texture.format ) ) {
                        output_creation.resource_info.texture.clear_values[ 0 ] = pass_output.value( "clear_depth", 1.0f );
                        output_creation.resource_info.texture.clear_values[ 1 ] = pass_output.value( "clear_stencil", 0.0f );
                    }
                    else {
                        // Parse color array
                        json clear_color_array = pass_output[ "clear_color" ];
                        if ( clear_color_array.is_array() ) {
                            for ( u32 c = 0; c < clear_color_array.size(); ++c) {
                                output_creation.resource_info.texture.clear_values[ c ] = clear_color_array[ c ];
                            }
                        }
                        else {
                            if ( output_creation.resource_info.texture.load_op == RenderPassOperation::Clear ) {
                                rprint( "Error parsing output texture %s: load operation is clear, but clear color not specified. Defaulting to 0,0,0,0.\n", output_creation.name );
                            }
                            output_creation.resource_info.texture.clear_values[ 0 ] = 0.0f;
                            output_creation.resource_info.texture.clear_values[ 1 ] = 0.0f;
                            output_creation.resource_info.texture.clear_values[ 2 ] = 0.0f;
                            output_creation.resource_info.texture.clear_values[ 3 ] = 0.0f;
                        }
                    }

                } break;
                case FrameGraphResourceType_Buffer:
                {
                    // NOTE(marco): for now buffers are all managed manually. We add them to the graph
                    // to make sure they are considered when performing the topological sort
                } break;
            }

            node_outputs.push( output_creation );
        }

        node_creation.inputs = Span<const FrameGraphResourceCreation>( node_inputs.data, node_inputs.size );
        node_creation.outputs = Span<const FrameGraphResourceCreation>( node_outputs.data, node_outputs.size );

        name_value = pass.value( "name", "" );
        RASSERT( !name_value.empty() );

        bool enabled = pass.value( "enabled", true );

        node_creation.name = string_buffer.append_use_f( "%s", name_value.c_str() );
        node_creation.enabled = enabled;

        FrameGraphNodeHandle node_handle = builder->create_node( node_creation );
        all_nodes.push( node_handle );
    }

    temp_allocator->free_marker( current_allocator_marker );
}

static void compute_edges( FrameGraph* frame_graph, FrameGraphNode* node, u32 node_index ) {

    FrameGraphNodeHandle node_handle = frame_graph->all_nodes[ node_index ];

    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        FrameGraphResource* resource = frame_graph->access_resource( node->inputs[ r ] );

        {
            FrameGraphResource* output_resource = frame_graph->get_resource( resource->name );
            if ( output_resource == nullptr && !resource->resource_info.external ) {
                // TODO(marco): external resources
                rprint( "Requested resource %s is not produced by any node and is not external.\n", resource->name );
                continue;
            }

            resource->producer = output_resource->producer;
            resource->resource_info = output_resource->resource_info;
            resource->output_handle = output_resource->output_handle;
        }

        for ( u32 n = 0; n < frame_graph->all_nodes.size; ++n ) {
            if ( n == node_index ) {
                continue;
            }

            FrameGraphNodeHandle parent_handle = frame_graph->all_nodes[ n ];
            FrameGraphNode* producer_node = frame_graph->access_node( parent_handle );

            for ( u32 o = 0; o < producer_node->outputs.size; ++o ) {
                FrameGraphResource* output_resource = frame_graph->access_resource( producer_node->outputs[ o ] );

                if ( strcmp( resource->name, output_resource->name ) != 0 ) {
                    continue;
                }

#if FRAME_GRAPH_DEBUG
                rprint( "Adding edge for resource %s from %s [%d] to %s [%d]\n", output_resource->name, producer_node->name, n, node->name, node_index )
#endif

                producer_node->edges.push( node_handle );
            }
        }
    }
}

static void compute_edges_v2( FrameGraph* frame_graph, FrameGraphNodeHandle node_handle ) {
    FrameGraphNode* node = frame_graph->access_node( node_handle );

    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        FrameGraphResource* resource = frame_graph->access_resource( node->inputs[ r ] );
        RASSERT( resource );

        FrameGraphNode* producer_node = frame_graph->access_node( resource->producer );
        RASSERTM( producer_node != node, "Node %s reads resource %s that it also produces", node->name, resource->name );

#if FRAME_GRAPH_DEBUG
        rprint( "Adding edge for resource %s from %s [%d] to %s [%d]\n", resource->name, producer_node->name, r, node->name, node_handle )
#endif

        producer_node->edges.push( node_handle );
    }
}

static void cache_node_output_textures( FrameGraph* frame_graph, FrameGraphNode* node ) {

    u32 width = 0;
    u32 height = 0;
    f32 scale_width = 0.f;
    f32 scale_height = 0.f;

    node->output_image_views.clear();
    node->output_depth_image_view = ImageViewHandle();
    node->output_shading_rate = ImageViewHandle();

    for ( u32 r = 0; r < node->outputs.size; ++r ) {
        FrameGraphResource* resource = frame_graph->access_resource( node->outputs[ r ] );
        FrameGraphResourceType output_type = resource->type;
        if ( node->version == 2 ) {
            resource = frame_graph->access_output_resource( node->outputs[ r ] );
        }

        FrameGraphResourceInfo& info = resource->resource_info;

        if ( output_type != FrameGraphResourceType_Attachment ) {
            continue;
        }

        if ( width == 0 ) {
            width = info.texture.width;
            scale_width = info.texture.scale_width > 0.f ? info.texture.scale_width : 1.f;
        } else {
            RASSERT( width == info.texture.width );
        }

        if ( height == 0 ) {
            height = info.texture.height;
            scale_height = info.texture.scale_height > 0.f ? info.texture.scale_height: 1.f;
        } else {
            RASSERT( height == info.texture.height );
        }

        if ( TextureFormat::has_depth( info.texture.format ) ) {
            node->output_depth_image_view = info.texture.image_view;
        } else {
            node->output_image_views.push( info.texture.image_view );
        }
    }

    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        FrameGraphResource* input_resource = frame_graph->access_output_resource( node->inputs[ r ] );

        FrameGraphResourceType input_type = node->version == 2 ? node->temp_inputs_type[ r ] : input_resource->type;

        if ( input_type != FrameGraphResourceType_Attachment && input_type != FrameGraphResourceType_ShadingRate ) {
            continue;
        }

        FrameGraphResource* resource = frame_graph->get_resource( input_resource->name );
        if ( node->version == 2 ) {
            resource = frame_graph->access_output_resource( resource->output_handle );
        }

        if ( resource == nullptr ) {
            continue;
        }

        FrameGraphResourceInfo& info = resource->resource_info;

        input_resource->resource_info.texture.image_view = info.texture.image_view;

        if ( width == 0 ) {
            width = info.texture.width;
            scale_width = info.texture.scale_width > 0.f ? info.texture.scale_width : 1.f;
        } else if ( input_type != FrameGraphResourceType_ShadingRate ) {
            RASSERT( width == info.texture.width );
        }

        if ( height == 0 ) {
            height = info.texture.height;
            scale_height = info.texture.scale_height > 0.f ? info.texture.scale_height : 1.f;
        } else if ( input_type != FrameGraphResourceType_ShadingRate ) {
            RASSERT( height == info.texture.height );
        }

        if ( input_type == FrameGraphResourceType_Texture ) {
            continue;
        }

        if ( resource->type == FrameGraphResourceType_ShadingRate ) {
            node->output_shading_rate = info.texture.image_view;

            continue;
        }

        if ( TextureFormat::has_depth( info.texture.format ) ) {
            node->output_depth_image_view = info.texture.image_view;
        } else {
            node->output_image_views.push( info.texture.image_view );
        }
    }

    node->resolution_scale_width = scale_width;
    node->resolution_scale_height = scale_height;
}

static void cache_node_render_pass_output( FrameGraph* frame_graph, FrameGraphNode* node ) {
    RenderPassOutput& render_pass_output = node->render_pass_output;
    render_pass_output = {};

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( u32 i = 0; i < node->outputs.size; ++i ) {
        FrameGraphResource* output_resource = frame_graph->access_resource( node->outputs[ i ] );
        FrameGraphResourceType output_type = output_resource->type;

        if ( node->version == 2 ) {
            output_resource = frame_graph->access_output_resource( node->outputs[ i ] );
        }

        FrameGraphResourceInfo& info = output_resource->resource_info;

        if ( output_type == FrameGraphResourceType_Attachment ) {
            if ( TextureFormat::has_depth( info.texture.format ) ) {
                render_pass_output.depth_stencil_format = info.texture.format;
                render_pass_output.depth_stencil_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                render_pass_output.depth_operation = VK_ATTACHMENT_LOAD_OP_CLEAR;
            } else {
                u32 rt_index = render_pass_output.num_color_formats;
                ++render_pass_output.num_color_formats;

                render_pass_output.color_formats[ rt_index ] = info.texture.format;
                render_pass_output.color_operations[ rt_index ] = info.texture.load_op;
                render_pass_output.color_final_layouts[ rt_index ] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
    }

    for ( u32 i = 0; i < node->inputs.size; ++i ) {
        FrameGraphResource* input_resource = frame_graph->access_resource( node->inputs[ i ] );
        if ( input_resource->type == FrameGraphResourceType_Reference ) {
            input_resource = frame_graph->access_resource( input_resource->output_handle );
        }

        FrameGraphResourceInfo& info = input_resource->resource_info;

        FrameGraphResourceType input_type = node->version == 2 ? node->temp_inputs_type[ i ] : input_resource->type;

        if ( input_type == FrameGraphResourceType_Attachment ) {
            if ( TextureFormat::has_depth( info.texture.format ) ) {
                render_pass_output.depth_stencil_format = info.texture.format;
                render_pass_output.depth_stencil_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                render_pass_output.depth_operation = VK_ATTACHMENT_LOAD_OP_LOAD;
            } else {
                u32 rt_index = render_pass_output.num_color_formats;
                ++render_pass_output.num_color_formats;

                render_pass_output.color_formats[ rt_index ] = info.texture.format;
                render_pass_output.color_operations[ rt_index ] = VK_ATTACHMENT_LOAD_OP_LOAD;
                render_pass_output.color_final_layouts[ rt_index ] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
    }

    // TODO(marco): make sure formats are valid for attachment
    // TODO(gabriel): see what is needed here.
    //node->render_pass_c = render_pass_creation;
}

void FrameGraph::enable_render_pass( cstring render_pass_name ) {
    FrameGraphNode* node = builder->get_node( render_pass_name );
    node->enabled = true;
}

void FrameGraph::disable_render_pass( cstring render_pass_name ) {
    FrameGraphNode* node = builder->get_node( render_pass_name );
    node->enabled = false;
}

namespace FrameGraphNodeVisitStatus {
    enum Enum {
        New = 0, Visited, Added, Count
    }; // enum Enum
}; // namespace FrameGraphNodeVisitStatus

template<typename T>
static void init_frame_graph_scratch_array( Array<T>& array, Allocator* allocator, u32 size ) {
    array.init( allocator, size, size );
    if ( size > 0 ) {
        memset( array.data, 0, array.size_in_bytes() );
    }
}

struct FrameGraphLayeringScratchEntry {
    u32  earliest_layer;
    u32  distance_to_sink;
    u32  latest_layer;
    u32  nearest_consumer_layer;
    bool active_node;
};

void FrameGraph::compile() {
    // TODO(marco)
    // - cull inactive nodes

    for ( u32 i = 0; i < all_nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( all_nodes[ i ] );

        // NOTE(marco): we want to clear all edges first, then populate them. If we clear them inside the loop
        // below we risk clearing the list after it has already been used by one of the child nodes
        node->edges.clear();
    }

    for ( u32 i = 0; i < all_nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( all_nodes[ i ] );
        if ( !node->enabled ) {
            continue;
        }

        if ( node->version == 2 ) {
            compute_edges_v2( this, all_nodes[ i ] );
        } else {
            compute_edges( this, node, i );
        }
    }

    Array<FrameGraphNodeHandle> sorted_nodes;
    sorted_nodes.init( &local_allocator, all_nodes.size );

    Array<u8> node_status;
    node_status.init( &local_allocator, all_nodes.size, all_nodes.size );
    memset( node_status.data, 0, sizeof( u8 ) * all_nodes.size );

    Array<FrameGraphNodeHandle> stack;
    stack.init( &local_allocator, nodes.size );

    // Topological sorting
    for ( u32 n = 0; n < all_nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( all_nodes[ n ] );
        if ( !node->enabled ) {
            continue;
        }

        stack.push( all_nodes[ n ] );

        while ( stack.size > 0 ) {
            FrameGraphNodeHandle node_handle = stack.back();

            if ( node_status[ node_handle.index ] == FrameGraphNodeVisitStatus::Added ) {
                stack.pop();

                continue;
            }

            if ( node_status[ node_handle.index ]  == FrameGraphNodeVisitStatus::Visited ) {
                node_status[ node_handle.index ] = FrameGraphNodeVisitStatus::Added;

                sorted_nodes.push( node_handle );

                stack.pop();

                continue;
            }

            node_status[ node_handle.index ] = FrameGraphNodeVisitStatus::Visited;

            FrameGraphNode* node = builder->access_node( node_handle );

            // Leaf node
            if ( node->edges.size == 0 ) {
                continue;
            }

            for ( u32 r = 0; r < node->edges.size; ++r ) {
                FrameGraphNodeHandle child_handle = node->edges[ r ];

                if ( node_status[ child_handle.index ] == FrameGraphNodeVisitStatus::New ) {
                    stack.push( child_handle );
                }
            }
        }
    }

    nodes.clear();
    persistent_update_nodes.clear();

    u32 max_node = 0;
    for ( i32 i = sorted_nodes.size - 1; i >= 0; --i ) {
#if FRAME_GRAPH_DEBUG
        FrameGraphNode* node = builder->access_node( sorted_nodes[ i ] );
        rprint( "Node %s is at position %d\n", node->name, nodes.size );
#endif

        nodes.push( sorted_nodes[ i ] );
        max_node = max( max_node, sorted_nodes[ i ].index );
    }

    node_status.shutdown();
    stack.shutdown();
    sorted_nodes.shutdown();

    node_layer.init( allocator, max_node + 1, max_node + 1 );
    memset( node_layer.data, 0, node_layer.size_in_bytes() );

    ArenaAllocator* scratch_allocator = MemoryService::instance()->get_thread_allocator();
    const sizet scratch_marker = scratch_allocator->get_marker();

    // Calculate the earliest and latest legal layer for each node. The previous
    // sink-distance-only assignment could place a late, short branch in column
    // zero (for example debug_draw -> debug_mesh -> post processing), even
    // though that branch executes near the end of the frame.
    const u32 scratch_node_count = max_node + 1;
    Array<FrameGraphLayeringScratchEntry> scratch;
    init_frame_graph_scratch_array( scratch, scratch_allocator, scratch_node_count );

    for ( u32 n = 0; n < nodes.size; ++n ) {
        const FrameGraphNodeHandle node_handle = nodes[ n ];
        scratch[ node_handle.index ].active_node = true;
    }

    // Relax the constraints instead of relying on the order returned by the
    // frame graph's DFS. Shared branches can make that order unsuitable for a
    // single reverse pass even though the dependency graph itself is valid.
    for ( u32 iteration = 0; iteration < nodes.size; ++iteration ) {
        bool changed = false;

        for ( u32 n = 0; n < nodes.size; ++n ) {
            const FrameGraphNodeHandle node_handle = nodes[ n ];
            FrameGraphNode* node = access_node( node_handle );

            for ( u32 input_index = 0; input_index < node->inputs.size; ++input_index ) {
                FrameGraphResource* input_resource = access_resource( node->inputs[ input_index ] );
                const FrameGraphNodeHandle producer = input_resource->producer;

                if ( producer.index != k_invalid_index && producer.index != node_handle.index &&
                     scratch[ producer.index ].active_node ) {
                    const u32 required_layer = scratch[ producer.index ].earliest_layer + 1;
                    if ( scratch[ node_handle.index ].earliest_layer < required_layer ) {
                        scratch[ node_handle.index ].earliest_layer = required_layer;
                        changed = true;
                    }
                }
            }
        }

        if ( !changed ) {
            break;
        }
    }

    // Derive the reverse constraint from the exact resource producers rather
    // than the aggregated edge list. This keeps versioned resources and the
    // links shown by the editor on the same dependency graph.
    for ( u32 iteration = 0; iteration < nodes.size; ++iteration ) {
        bool changed = false;

        for ( u32 n = 0; n < nodes.size; ++n ) {
            const FrameGraphNodeHandle node_handle = nodes[ n ];
            FrameGraphNode* node = access_node( node_handle );

            for ( u32 input_index = 0; input_index < node->inputs.size; ++input_index ) {
                FrameGraphResource* input_resource = access_resource( node->inputs[ input_index ] );
                const FrameGraphNodeHandle producer = input_resource->producer;

                if ( producer.index != k_invalid_index && producer.index != node_handle.index &&
                     scratch[ producer.index ].active_node ) {
                    const u32 required_distance = scratch[ node_handle.index ].distance_to_sink + 1;
                    if ( scratch[ producer.index ].distance_to_sink < required_distance ) {
                        scratch[ producer.index ].distance_to_sink = required_distance;
                        changed = true;
                    }
                }
            }
        }

        if ( !changed ) {
            break;
        }
    }

    u32 maximum_layer = 0;
    for ( u32 n = 0; n < nodes.size; ++n ) {
        maximum_layer = max( maximum_layer, scratch[ nodes[ n ].index ].earliest_layer );
    }

    // Prefer the node's actual compiled execution position within its legal
    // interval. This moves late side branches to the right without violating
    // producer-before-consumer ordering.
    for ( u32 n = 0; n < nodes.size; ++n ) {
        const FrameGraphNodeHandle node_handle = nodes[ n ];
        scratch[ node_handle.index ].latest_layer = max( scratch[ node_handle.index ].earliest_layer,
                                                         maximum_layer - min( maximum_layer, scratch[ node_handle.index ].distance_to_sink ) );
        u32 preferred_layer = nodes.size > 1
            ? u32( ( u64( n ) * maximum_layer + ( nodes.size - 1 ) / 2 ) / ( nodes.size - 1 ) )
            : 0;

        // A node with dependencies but no consumers is a terminal pass, not a
        // graph source. Anchor it at the right edge; leave completely isolated
        // nodes free to follow their declaration/execution position.
        if ( scratch[ node_handle.index ].earliest_layer > 0 && scratch[ node_handle.index ].distance_to_sink == 0 ) {
            preferred_layer = maximum_layer;
        }

        node_layer[ node_handle.index ] = min( max( preferred_layer, scratch[ node_handle.index ].earliest_layer ),
                                               scratch[ node_handle.index ].latest_layer );
    }

    // A final forward relaxation makes the ordering invariant explicit even if
    // several nodes choose opposite ends of overlapping legal intervals.
    for ( u32 iteration = 0; iteration < nodes.size; ++iteration ) {
        bool changed = false;

        for ( u32 n = 0; n < nodes.size; ++n ) {
            const FrameGraphNodeHandle node_handle = nodes[ n ];
            FrameGraphNode* node = access_node( node_handle );

            for ( u32 input_index = 0; input_index < node->inputs.size; ++input_index ) {
                FrameGraphResource* input_resource = access_resource( node->inputs[ input_index ] );
                const FrameGraphNodeHandle producer = input_resource->producer;

                if ( producer.index != k_invalid_index && producer.index != node_handle.index &&
                     scratch[ producer.index ].active_node ) {
                    const u32 required_layer = node_layer[ producer.index ] + 1;
                    if ( node_layer[ node_handle.index ] < required_layer ) {
                        node_layer[ node_handle.index ] = required_layer;
                        changed = true;
                    }
                }
            }
        }

        if ( !changed ) {
            break;
        }
    }

    // Pull each producer toward its nearest consumer. Moving it to at most one
    // column before the earliest consumer preserves every dependency while
    // keeping chains such as transparent -> debug_draw -> debug_mesh compact.
    for ( u32 iteration = 0; iteration < nodes.size; ++iteration ) {
        for ( u32 n = 0; n < scratch.size; ++n ) {
            scratch[ n ].nearest_consumer_layer = k_invalid_index;
        }

        for ( u32 n = 0; n < nodes.size; ++n ) {
            const FrameGraphNodeHandle consumer_handle = nodes[ n ];
            FrameGraphNode* consumer = access_node( consumer_handle );

            for ( u32 input_index = 0; input_index < consumer->inputs.size; ++input_index ) {
                FrameGraphResource* input_resource = access_resource( consumer->inputs[ input_index ] );
                const FrameGraphNodeHandle producer = input_resource->producer;

                if ( producer.index != k_invalid_index && producer.index != consumer_handle.index &&
                     scratch[ producer.index ].active_node ) {
                    scratch[ producer.index ].nearest_consumer_layer = min( scratch[ producer.index ].nearest_consumer_layer,
                                                                            node_layer[ consumer_handle.index ] );
                }
            }
        }

        bool changed = false;
        for ( u32 n = 0; n < nodes.size; ++n ) {
            const FrameGraphNodeHandle node_handle = nodes[ n ];
            const u32 consumer_layer = scratch[ node_handle.index ].nearest_consumer_layer;

            if ( consumer_layer != k_invalid_index && consumer_layer > 0 ) {
                const u32 compacted_layer = min( consumer_layer - 1, scratch[ node_handle.index ].latest_layer );
                if ( node_layer[ node_handle.index ] < compacted_layer ) {
                    node_layer[ node_handle.index ] = compacted_layer;
                    changed = true;
                }
            }
        }

        if ( !changed ) {
            break;
        }
    }

    scratch_allocator->free_marker( scratch_marker );

    // NOTE(marco): allocations and deallocations are used for verification purposes only
    u32 resource_count = builder->resource_cache.resources.used_indices;
    Array<FrameGraphNodeHandle> allocations;
    allocations.init( &local_allocator, resource_count, resource_count );
    for ( u32 i = 0; i < resource_count; ++i) {
        allocations[ i ].index = k_invalid_index;
    }

    Array<FrameGraphNodeHandle> deallocations;
    deallocations.init( &local_allocator, resource_count, resource_count );
    for ( u32 i = 0; i < resource_count; ++i) {
        deallocations[ i ].index = k_invalid_index;
    }

    Array<ImageHandle> free_list;
    free_list.init( &local_allocator, resource_count );

    size_t peak_memory = 0;
    size_t instant_memory = 0;

    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        if ( !node->enabled ) {
            continue;
        }

        for ( u32 j = 0; j < node->inputs.size; ++j ) {
            FrameGraphResource* input_resource = builder->access_resource( node->inputs[ j ] );
            FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

            if ( resource == nullptr ) {
                continue;
            }

            resource->ref_count++;
        }
    }

    // Create resources
    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        if ( !node->enabled ) {
            continue;
        }

        bool add_to_persistent_nodes = false;

        for ( u32 j = 0; j < node->outputs.size; ++j ) {
            FrameGraphResourceHandle output_handle = node->outputs[ j ];
            u32 resource_index = output_handle.index;
            FrameGraphResource* resource = builder->access_resource( output_handle );

            if ( !resource->resource_info.external && allocations[ resource_index ].index == k_invalid_index ) {
                RASSERT( deallocations[ resource_index ].index == k_invalid_index )
                allocations[ resource_index ] = nodes[ i ];

                if ( resource->type == FrameGraphResourceType_Attachment ) {
                    if ( resource->output_handle.index != output_handle.index ) {
                        // This resource has already been allocated by the first node that produced it
                        continue;
                    }

                    FrameGraphResourceInfo& info = resource->resource_info;

                    // Resolve texture size if needed
                    if ( info.texture.width == 0 || info.texture.height == 0 ) {

                        if ( builder->resolution_info ) {
                            info.texture.width = u32( builder->resolution_info->render_width * info.texture.scale_width );
                            info.texture.height = u32( builder->resolution_info->render_height * info.texture.scale_height );
                        }
                        else {
                            info.texture.width = u32( builder->device->swapchain_width * info.texture.scale_width );
                            info.texture.height = u32( builder->device->swapchain_height * info.texture.scale_height );
                        }
                    }

                    // Frame graph resources are always render targets that can also be
                    // sampled, and the aliasing pass blits between them - hence the two
                    // transfer bits. A depth format is a depth attachment, never a color one.
                    const bool is_depth_resource = TextureFormat::has_depth_or_stencil( info.texture.format );
                    VkImageUsageFlags texture_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                      ( is_depth_resource ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT );
                    texture_usage |= info.texture.compute ? VK_IMAGE_USAGE_STORAGE_BIT : 0;

                    bool found_suitable_free_resource = false;
                    // Avoid memory aliasing for persistent resources
                    if ( free_list.size > 0 && !info.texture.persistent ) {
                        for ( u32 r = 0; r < free_list.size; ++r ) {
                            ImageHandle alias_image_handle = free_list[ r ];
                            Image* alias_image = builder->device->get_image( alias_image_handle );

                            if ( alias_image->width != info.texture.width ||
                                 alias_image->height != info.texture.height ||
                                 alias_image->vk_format != info.texture.format ) {
                                continue;
                            }

                            // NOTE(marco): this texture has already been aliased, get original image
                            if ( alias_image->alias_image.is_valid() ) {
                                alias_image_handle = alias_image->alias_image;
                                alias_image = builder->device->get_image( alias_image_handle );
                            }

                            // TODO: a resource declared with depth > 1 is still created as a
                            // VK_IMAGE_TYPE_2D image here, exactly as before. That combination is
                            // invalid Vulkan and vulkan_validate_image_creation now says so.
                            ImageCreation texture_creation{
                                .image_type = VK_IMAGE_TYPE_2D,
                                .format     = info.texture.format,
                                .width      = info.texture.width,
                                .height     = info.texture.height,
                                .depth      = info.texture.depth,
                                .usage      = texture_usage,
                                .alias      = alias_image_handle,
                                .name       = resource->name };
                            ImageHandle handle = builder->device->create_image( texture_creation );

                            VkImageAspectFlags image_view_flag = TextureFormat::has_depth( texture_creation.format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

                            ImageViewHandle view_handle = builder->device->create_image_view( {
                                .parent_image = handle,
                                .view_type = VK_IMAGE_VIEW_TYPE_2D,
                                .sub_resource = { image_view_flag, 0, 1, 0, 1 },
                                .name = resource->name } );

                            builder->device->add_image_view_to_bindless( view_handle );

                            info.texture.image = handle;
                            info.texture.image_view = view_handle;

                            free_list.delete_swap( r );
                            found_suitable_free_resource = true;
                            break;
                        }
                    }

                    if ( !found_suitable_free_resource ) {
                        ImageCreation image_creation{
                            .image_type = VK_IMAGE_TYPE_2D,
                            .format     = info.texture.format,
                            .width      = info.texture.width,
                            .height     = info.texture.height,
                            .depth      = info.texture.depth,
                            .usage      = texture_usage,
                            .name       = resource->name };
                        ImageHandle handle = builder->device->create_image( image_creation );

                        VkImageAspectFlags image_view_flag = TextureFormat::has_depth( image_creation.format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

                        ImageViewCreation image_view_creation = {
                            .parent_image = handle,
                            .view_type = VK_IMAGE_VIEW_TYPE_2D,
                            .sub_resource = { image_view_flag, 0, 1, 0, 1 },
                            .name = resource->name };
                        ImageViewHandle view_handle = builder->device->create_image_view( image_view_creation );

                        builder->device->add_image_view_to_bindless( view_handle );

                        info.texture.image = handle;
                        info.texture.image_view = view_handle;

                        // Add previous image and image view
                        if ( info.texture.persistent ) {
                            image_creation.name = builder->rename_buffer.append_use_f( "%s_previous", resource->name );
                            info.texture.previous_image = builder->device->create_image( image_creation );

                            image_view_creation.parent_image = info.texture.previous_image;
                            image_view_creation.name = image_creation.name;
                            info.texture.previous_image_view = builder->device->create_image_view( image_view_creation );

                            builder->device->add_image_view_to_bindless( info.texture.previous_image_view );

                            add_to_persistent_nodes = true;
                        }
                    }
                }

#if FRAME_GRAPH_DEBUG
                rprint( "Output %s allocated on node %d\n", resource->name, nodes[ i ].index );
#endif
            }
        }

        for ( u32 j = 0; j < node->inputs.size; ++j ) {
            FrameGraphResource* input_resource = builder->access_resource( node->inputs[ j ] );

            u32 resource_index = input_resource->output_handle.index;
            FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

            if ( resource == nullptr ) {
                continue;
            }

            resource->ref_count--;

            if ( resource->resource_info.texture.persistent ) {
                add_to_persistent_nodes = true;
            }

            if ( !resource->resource_info.external && resource->ref_count == 0 ) {
                RASSERT( deallocations[ resource_index ].index == k_invalid_index );
                deallocations[ resource_index ] = nodes[ i ];

                if ( (resource->type == FrameGraphResourceType_Attachment || resource->type == FrameGraphResourceType_Texture) &&
                     !(resource->resource_info.texture.persistent || resource->resource_info.texture.disable_memory_aliasing ) ) {

                    ImageViewHandle image_view = resource->resource_info.texture.image_view;
                    ImageView* view = builder->device->get_image_view( image_view );
                    Image* image = builder->device->get_image( view->parent_image );

                    free_list.push( image->handle );
                }

#if FRAME_GRAPH_DEBUG
                rprint( "Output %s deallocated on node %d\n", resource->name, nodes[ i ].index );
#endif
            }
        }


        if ( add_to_persistent_nodes ) {
            persistent_update_nodes.push( nodes[ i ] );
        }
    }

    allocations.shutdown();
    deallocations.shutdown();
    free_list.shutdown();

    // Cache output and textures used to begin pass command
    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        RASSERT( node->enabled );

        if ( node->compute ) {
            continue;
        }

        if ( !node->render_pass_output_cached ) {
            cache_node_render_pass_output( this, node );
            node->render_pass_output_cached = true;
        }

        if ( !node->output_textures_cached ) {
            cache_node_output_textures( this, node );
            node->output_textures_cached = true;
        }
    }

    // This should not be needed here, but just in case: free memory
    for ( u32 i = 0; i < batches.size; i++ ) {
        FrameGraphExecutionBatch& batch = batches[ i ];
        batch.nodes.shutdown();
    }
    batches.clear();

    // Calculate batches
    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        RASSERT( node->enabled );

        // Search for corresponding batch
        FrameGraphExecutionBatch* batch = nullptr;
        for ( u32 b = 0; b < batches.size; ++b ) {
            FrameGraphExecutionBatch* current_batch = &batches[ b ];

            if ( current_batch->scheduling.queue_type == node->scheduling.queue_type &&
                 current_batch->scheduling.phase == node->scheduling.phase ) {
                batch = current_batch;
                break;
            }
        }

        // If none found, add a new one
        if ( batch == nullptr ) {
            batch = &batches.push_use();

            batch->scheduling = node->scheduling;
            batch->nodes.init( allocator, 8 );
            batch->cb = nullptr;
        }

        // Add node to the batch
        RASSERT( batch );

        batch->nodes.push( nodes[ i ] );
    }
}

void FrameGraph::add_ui() {
    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        RASSERT( node->enabled );

        node->graph_render_pass->add_ui();
    }
}

void FrameGraph::render( u32 frame_index, u32 thread_index,
                         Renderer* renderer, RenderView* render_view,
                         RenderBlackboard* render_blackboard,
                         RenderConfig* render_config ) {

    RASSERTM( per_frame_persistent_update_called == true, "Forgot to call update_persistent_resources_handles after this method in previous frame!" );
    RenderScene* render_scene = render_view->scene;
    GpuProfiler* gpu_profiler = renderer->gpu->gpu_profiler;

    // Allocate and begin command buffers
    for ( u32 b = 0; b < batches.size; ++b ) {

        FrameGraphExecutionBatch& batch = batches[ b ];
        batch.cb = renderer->gpu->allocate_command_buffer( thread_index, frame_index, batch.scheduling.queue_type );

        batch.cb->begin();
        gpu_profiler->begin_command_buffer( batch.cb );

        // Frame Graph Marker
        batch.cb->push_marker( "Frame Graph" );
    }

    for ( u32 b = 0; b < batches.size; ++b ) {

        FrameGraphExecutionBatch& batch = batches[ b ];

        for ( u32 n = 0; n < batch.nodes.size; ++n ) {
            ZoneScopedN( "RenderPass" );

            FrameGraphNode* node = builder->access_node( batch.nodes[ n ] );
            RASSERT( node->enabled );

            CommandBuffer* cb = batch.cb;
            FrameGraphRenderContext render_context{ renderer, cb, this, render_view, render_blackboard, render_config, frame_index };

            //rprint( "%s\n", node->name );
            cb->push_marker( node->name );

            // Compute Node
            if ( node->compute ) {

                for ( u32 i = 0; i < node->inputs.size; ++i ) {
                    FrameGraphResource* input_resource = builder->access_resource( node->inputs[ i ] );
                    FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

                    if ( resource == nullptr || resource->resource_info.external ) {
                        continue;
                    }

                    if ( input_resource->type == FrameGraphResourceType_Texture ) {
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        const VkImageAspectFlags aspect = TextureFormat::has_depth( texture->vk_format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

                        cb->add_image_barrier( texture->handle, range_aspect( aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                               { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                 VK_ACCESS_2_SHADER_READ_BIT,
                                               VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
                        //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_SHADER_RESOURCE, 0, 1, TextureFormat::has_depth(texture->vk_format) );
                    } else if ( input_resource->type == FrameGraphResourceType_Attachment ) {
                        //// TODO: what to do with attachments ?
                        //Texture* texture = cb->gpu_device->access_texture( resource->resource_info.texture.handle );
                        //texture = texture;
                    }
                }

                for ( u32 o = 0; o < node->outputs.size; ++o ) {
                    FrameGraphResource* resource = builder->access_resource( node->outputs[ o ] );

                    if ( resource->type == FrameGraphResourceType_Attachment ) {
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        if ( TextureFormat::has_depth( texture->vk_format ) ) {
                            // Is this supported even ?
                            RASSERT( false );
                        } else {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                                   { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                                                     VK_IMAGE_LAYOUT_GENERAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1, false );
                        }
                    }
                }

                cb->flush_barriers();

                node->graph_render_pass->pre_render( render_context );
                node->graph_render_pass->render( render_context );
                node->graph_render_pass->post_render( render_context );

            } else if ( node->ray_tracing ) {
                // Ray-Tracing Node
                node->graph_render_pass->pre_render( render_context );
                node->graph_render_pass->render( render_context );
                node->graph_render_pass->post_render( render_context );

            } else {
                // Graphics Node
                u32 width = 0;
                u32 height = 0;

                node->graph_render_pass->pre_render( render_context );

                // Add input barriers
                for ( u32 i = 0; i < node->inputs.size; ++i ) {
                    FrameGraphResource* input_resource = builder->access_resource( node->inputs[ i ] );
                    FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

                    if ( resource == nullptr || resource->resource_info.external ) {
                        continue;
                    }

                    FrameGraphResourceType input_type = node->version == 2 ? node->temp_inputs_type[ i ] : input_resource->type;

                    if ( input_type == FrameGraphResourceType_Texture ) {
                        ImageView* image_view = cb->gpu_device->get_image_view( resource->resource_info.texture.image_view );
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        const VkImageAspectFlags aspect = TextureFormat::has_depth( texture->vk_format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                        cb->add_image_barrier( texture->handle, range_aspect( aspect, 0, 1, 0, 1 ),
                                               { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                 VK_ACCESS_2_SHADER_READ_BIT,
                                                 VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
                        //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 0, 1, TextureFormat::has_depth( texture->vk_format ) );
                    } else if ( input_type == FrameGraphResourceType_Attachment ) {
                        ImageView* image_view = cb->gpu_device->get_image_view( resource->resource_info.texture.image_view );
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        width = texture->width;
                        height = texture->height;

                        // For textures that are read-write check if a transition is needed.
                        if ( !TextureFormat::has_depth_or_stencil( texture->vk_format ) ) {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_RENDER_TARGET, 0, 1, false );
                        } else {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_DEPTH_WRITE, 0, 1, true );
                        }
                    }
                }

                cb->flush_barriers();

                // Add output barriers and collect pass data
                ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
                u64 marker = temp_allocator->get_marker();

                for ( u32 o = 0; o < node->outputs.size; ++o ) {
                    FrameGraphResource* resource = builder->access_resource( node->outputs[ o ] );

                    if ( resource->type == FrameGraphResourceType_Attachment ) {
                        if ( node->version == 2 ) {
                            for ( u32 i = 0; i < node->inputs.size; ++i ) {
                                FrameGraphResource* input_resource = builder->access_resource( node->inputs[ i ] );
                                if ( input_resource->output_handle.index == resource->output_handle.index ) {
                                    // This resource is also an input, skip
                                    continue;
                                }
                            }
                        }

                        ImageView* image_view = cb->gpu_device->get_image_view( resource->resource_info.texture.image_view );
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        width = texture->width;
                        height = texture->height;

                        // Depth texture
                        if ( TextureFormat::has_depth( texture->vk_format ) ) {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_DEPTH_WRITE, 0, 1, true );
                            cb->flush_barriers();
                            f32* clear_color = resource->resource_info.texture.clear_values;
                            cb->clear_depth_stencil( clear_color[ 0 ], (u8)clear_color[ 1 ] );

                        } else {
                            // Color texture
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_RENDER_TARGET, 0, 1, false );
                            cb->flush_barriers();
                            f32* clear_color = resource->resource_info.texture.clear_values;
                            cb->clear( clear_color[ 0 ], clear_color[ 1 ], clear_color[ 2 ], clear_color[ 3 ], o );
                        }
                    }
                }

                /*Rect2DInt scissor{ 0, 0,( u16 )width, ( u16 )height };
                cb->set_scissor( &scissor );

                Viewport viewport{ };
                viewport.rect = { 0, 0, ( u16 )width, ( u16 )height };
                viewport.min_depth = 0.0f;
                viewport.max_depth = 1.0f;

                cb->set_viewport( &viewport );*/

                if ( cb->gpu_device->fragment_shading_rate_present ) {
                    cb->set_shading_rate( 1, 1 );
                }

                // Cache needed spans
                const u32 num_textures = node->output_image_views.size;
                Span<const ImageViewHandle> rts( node->output_image_views.data, num_textures );
                Span<const VkAttachmentLoadOp> loads( node->render_pass_output.color_operations, num_textures );
                Span<const VkClearValue> clear_colors( cb->clear_values, num_textures );

                cb->begin_render_pass( rts, loads, clear_colors,
                                       node->output_depth_image_view,
                                       node->render_pass_output.depth_operation,
                                       cb->clear_values[ CommandBuffer::k_depth_stencil_clear_index ],
                                       node->output_shading_rate,
                                       1, node->render_pass_output.multiview_mask );

                cb->set_fullscreen_scissor();
                cb->set_fullscreen_viewport();

                node->graph_render_pass->render( render_context );

                cb->end_render_pass();

                node->graph_render_pass->post_render( render_context );
            }

            cb->pop_marker();
            //gpu_profiler->pop_timestamp( cb );
        }
    }

    for ( u32 b = 0; b < batches.size; ++b ) {

        FrameGraphExecutionBatch& batch = batches[ b ];

        //gpu_profiler->pop_timestamp( batch.cb );
        batch.cb->pop_marker();

        gpu_profiler->end_command_buffer( batch.cb );
        batch.cb->end();
    }
}

void FrameGraph::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard,
                            RenderConfig* render_config, u32 new_width, u32 new_height ) {

    GpuDevice& gpu = *renderer->gpu;

    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        RASSERT( node->enabled );

        for ( u32 r = 0; r < node->outputs.size; ++r ) {
            FrameGraphResource* resource = access_resource( node->outputs[ r ] );

            FrameGraphResourceInfo& info = resource->resource_info;

            // Avoid attachments or external resources
            if ( resource->type != FrameGraphResourceType_Attachment ||
                 resource->resource_info.external ) {
                continue;
            }

            if ( info.texture.scale_height == 0.0f || info.texture.scale_width == 0.0f ) {
                info.texture.width = new_width;
                info.texture.height = new_height;
            } else {
                info.texture.width = u32( new_width * info.texture.scale_width );
                info.texture.height = u32( new_height * info.texture.scale_height );
            }

            gpu.resize_image( info.texture.image, info.texture.width, info.texture.height );
            gpu.recreate_image_view( info.texture.image_view );
            gpu.add_image_view_to_bindless( info.texture.image_view );

            if ( info.texture.persistent ) {
                gpu.resize_image( info.texture.previous_image, info.texture.width, info.texture.height );
                gpu.recreate_image_view( info.texture.previous_image_view );
                gpu.add_image_view_to_bindless( info.texture.previous_image_view );
            }
        }

        FrameGraphResourceContext resource_context{ renderer, this, render_blackboard, render_config, nullptr };
        node->graph_render_pass->on_resize( resource_context, new_width, new_height );
    }
}

void FrameGraph::reload_shaders( RenderScene& scene, RenderBlackboard* render_blackboard, RenderConfig* render_config ) {

    FrameGraphResourceContext resource_context{ scene.renderer, this, render_blackboard, render_config, &scene };

    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        RASSERT( node->enabled );

        node->graph_render_pass->update_psos( resource_context, PipelineUpdatePhase::Reload );
    }
}

void FrameGraph::update_persistent_resources_handles() {

    RASSERT( per_frame_persistent_update_called );

    for ( u32 i = 0; i < persistent_update_nodes.size; i++ ) {
        FrameGraphNode* node = builder->access_node( persistent_update_nodes[ i ] );

        // Flip current/previous image and image view nodes
        for ( u32 j = 0; j < node->outputs.size; ++j ) {
            FrameGraphResourceHandle output_handle = node->outputs[ j ];
            FrameGraphResource* resource = builder->access_resource( output_handle );
            if ( !resource->resource_info.texture.persistent ) {
                continue;
            }

            FrameGraphResourceInfo& info = resource->resource_info;
            // Cache what is now "previous"
            ImageHandle previous_image = info.texture.image;
            ImageViewHandle previous_image_view = info.texture.image_view;
            // Flip current with previous
            info.texture.image = info.texture.previous_image;
            info.texture.image_view = info.texture.previous_image_view;

            info.texture.previous_image = previous_image;
            info.texture.previous_image_view = previous_image_view;
        }

        cache_node_output_textures( this, node );
    }

    per_frame_persistent_update_called = true;
}

CommandBuffer* FrameGraph::get_command_buffer_from_batch( CommandQueueType queue, u32 phase ) {

    for ( u32 b = 0; b < batches.size; ++b ) {
        FrameGraphExecutionBatch& batch = batches[ b ];

        if ( batch.scheduling.queue_type == queue &&
             batch.scheduling.phase == phase ) {
            return batch.cb;
        }
    }

    RASSERT( false );
    return nullptr;
}

static constexpr f32 k_frame_graph_pin_radius = 4.0f;

static ed::NodeId frame_graph_node_id( FrameGraphNodeHandle handle ) {
    // Zero is the node editor's invalid id, while frame graph handles start at zero.
    return handle.index + 1;
}

static constexpr u32 k_frame_graph_pin_id_stride = FrameGraphBuilder::k_max_resources_count;
static constexpr u32 k_frame_graph_input_pin_id_base = FrameGraphBuilder::k_max_nodes_count + 1;
static constexpr u32 k_frame_graph_output_pin_id_base =
    k_frame_graph_input_pin_id_base + FrameGraphBuilder::k_max_nodes_count * k_frame_graph_pin_id_stride;
static constexpr u32 k_frame_graph_link_id_base =
    k_frame_graph_output_pin_id_base + FrameGraphBuilder::k_max_nodes_count * k_frame_graph_pin_id_stride;

static ed::PinId frame_graph_input_pin_id( FrameGraphNodeHandle node, u32 input_index ) {
    return k_frame_graph_input_pin_id_base + node.index * k_frame_graph_pin_id_stride + input_index;
}

static ed::PinId frame_graph_output_pin_id( FrameGraphNodeHandle node, u32 output_index ) {
    return k_frame_graph_output_pin_id_base + node.index * k_frame_graph_pin_id_stride + output_index;
}

static ed::LinkId frame_graph_link_id( FrameGraphNodeHandle node, u32 input_index ) {
    return k_frame_graph_link_id_base + node.index * k_frame_graph_pin_id_stride + input_index;
}

static f32 frame_graph_pin_width( cstring name ) {
    const f32 pin_diameter = k_frame_graph_pin_radius * 2.0f;
    return pin_diameter + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize( name ).x;
}

static bool draw_frame_graph_pin( cstring name, ed::PinId pin_id, ed::PinKind kind, bool selected ) {
    const f32 pin_diameter = k_frame_graph_pin_radius * 2.0f;
    const f32 line_height = ImGui::GetTextLineHeight();
    const ImVec2 pin_row_min = ImGui::GetCursorScreenPos();
    ImVec2 pin_center;

    ed::BeginPin( pin_id, kind );

    if ( kind == ed::PinKind::Input ) {
        const ImVec2 pin_position = ImGui::GetCursorScreenPos();
        pin_center = ImVec2( pin_position.x + k_frame_graph_pin_radius, pin_position.y + line_height * 0.5f );

        ImGui::Dummy( ImVec2( pin_diameter, line_height ) );
        ImGui::SameLine( 0.0f, ImGui::GetStyle().ItemInnerSpacing.x );
        ImGui::TextUnformatted( name );
    } else {
        ImGui::TextUnformatted( name );
        ImGui::SameLine( 0.0f, ImGui::GetStyle().ItemInnerSpacing.x );

        const ImVec2 pin_position = ImGui::GetCursorScreenPos();
        pin_center = ImVec2( pin_position.x + k_frame_graph_pin_radius, pin_position.y + line_height * 0.5f );
        ImGui::Dummy( ImVec2( pin_diameter, line_height ) );
    }

    ed::PinPivotRect( pin_center, pin_center );
    ed::EndPin();

    const ImVec2 last_item_max = ImGui::GetItemRectMax();
    const ImVec2 pin_row_max( last_item_max.x, pin_row_min.y + line_height );
    ImVec4 pin_color = ed::GetStyle().Colors[ selected ? ed::StyleColor_SelNodeBorder : ed::StyleColor_PinRectBorder ];
    pin_color.w = 1.0f;
    ImGui::GetWindowDrawList()->AddCircleFilled( pin_center, k_frame_graph_pin_radius, ImGui::ColorConvertFloat4ToU32( pin_color ) );

    return ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && ImGui::IsMouseHoveringRect( pin_row_min, pin_row_max );
}

static bool find_frame_graph_output_pin( FrameGraph* frame_graph, FrameGraphNodeHandle consumer_handle,
                                         u32 input_index, ed::PinId& output_pin_id ) {
    FrameGraphNode* consumer = frame_graph->access_node( consumer_handle );
    const FrameGraphResourceHandle input_handle = consumer->inputs[ input_index ];
    FrameGraphResource* input_resource = frame_graph->access_resource( input_handle );

    if ( input_resource->producer.index == k_invalid_index ) {
        return false;
    }

    // V2 inputs directly reference the producer's exact output version. V1 inputs
    // own a separate resource record patched to point at the producer output.
    const FrameGraphResourceHandle source_handle = consumer->version == 2 ? input_handle : input_resource->output_handle;
    FrameGraphNode* producer = frame_graph->access_node( input_resource->producer );

    for ( u32 output_index = 0; output_index < producer->outputs.size; ++output_index ) {
        if ( producer->outputs[ output_index ].index == source_handle.index ) {
            output_pin_id = frame_graph_output_pin_id( input_resource->producer, output_index );
            return true;
        }
    }

    return false;
}

static void select_frame_graph_debug_image( FrameGraph* frame_graph, FrameGraphResourceHandle selected_handle ) {
    FrameGraphResourceHandle resolved_handle = selected_handle;
    FrameGraphResource* resource = frame_graph->access_resource( resolved_handle );
    if ( resource == nullptr ) {
        return;
    }

    // Inputs and versioned outputs can be references. Follow them to the
    // resource that owns the physical image view used by the image viewer.
    while ( resource->output_handle.index != k_invalid_index &&
            resource->output_handle.index != resolved_handle.index ) {
        resolved_handle = resource->output_handle;
        resource = frame_graph->access_resource( resolved_handle );
        if ( resource == nullptr ) {
            return;
        }
    }

    if ( resource->type != FrameGraphResourceType_Texture &&
         resource->type != FrameGraphResourceType_Attachment &&
         resource->type != FrameGraphResourceType_ShadingRate &&
         resource->type != FrameGraphResourceType_Present ) {
        return;
    }

    const ImageViewHandle image_view = resource->resource_info.texture.image_view;
    if ( image_view.is_invalid() ) {
        return;
    }

    frame_graph->debug_image_resource = selected_handle;
    frame_graph->debug_image_view = image_view;
    frame_graph->debug_image_selection_pending = true;
}

struct FrameGraphLayoutScratchEntry {
    FrameGraphNodeHandle ordered_handle;
    f32                  barycenter;

    f32                  layer_width;
    f32                  layer_height;
    f32                  layer_x;
    f32                  layer_y;
    u32                  nodes_in_layer;
    u32                  layer_offset;
    u32                  layer_write_offset;

    u32                  node_order;
    f32                  node_y;
    f32                  node_width;
    f32                  node_height;
    f32                  avoidance_offset;
    bool                 node_is_in_layout;
};

static void sort_frame_graph_layout_layer( FrameGraphLayoutScratchEntry* layout, u32 begin, u32 end ) {
    // Layers are normally small, so stable insertion sort is sufficient and keeps
    // the previous order when two nodes have the same barycenter.
    for ( u32 i = begin + 1; i < end; ++i ) {
        const FrameGraphNodeHandle ordered_handle = layout[ i ].ordered_handle;
        const f32 barycenter = layout[ i ].barycenter;
        u32 insertion_index = i;

        while ( insertion_index > begin && layout[ insertion_index - 1 ].barycenter > barycenter ) {
            layout[ insertion_index ].ordered_handle = layout[ insertion_index - 1 ].ordered_handle;
            layout[ insertion_index ].barycenter = layout[ insertion_index - 1 ].barycenter;
            --insertion_index;
        }

        layout[ insertion_index ].ordered_handle = ordered_handle;
        layout[ insertion_index ].barycenter = barycenter;
    }
}

void FrameGraph::debug_ui() {

    ed::SetCurrentEditor(ed_context);

    const bool layout_requested = ImGui::Button( "Layout nodes" );

    ed::Begin( "Framegraph", ImVec2( 0.0, 0.0f ) );

    for ( u32 n = 0; n < nodes.size; ++n ) {
        const FrameGraphNodeHandle node_handle = nodes[ n ];
        FrameGraphNode* node = builder->access_node( node_handle );
        ed::BeginNode( frame_graph_node_id( node_handle ) );
        const f32 content_start_x = ImGui::GetCursorPosX();
        f32 input_column_width = 0.0f;
        f32 output_column_width = 0.0f;

        for ( u32 i = 0; i < node->inputs.size; ++i ) {
            FrameGraphResource* resource = access_resource( node->inputs[ i ] );
            input_column_width = max( input_column_width, frame_graph_pin_width( resource->name ) );
        }

        for ( u32 o = 0; o < node->outputs.size; ++o ) {
            FrameGraphResource* resource = builder->access_resource( node->outputs[ o ] );
            output_column_width = max( output_column_width, frame_graph_pin_width( resource->name ) );
        }

        const f32 column_gap = node->inputs.size > 0 && node->outputs.size > 0 ? ImGui::GetStyle().ItemSpacing.x * 4.0f : 0.0f;
        const f32 pins_width = input_column_width + column_gap + output_column_width;
        const f32 content_width = max( ImGui::CalcTextSize( node->name ).x, pins_width );

        ImGui::TextUnformatted( node->name );

        const u32 pin_rows = max( node->inputs.size, node->outputs.size );
        for ( u32 row = 0; row < pin_rows; ++row ) {
            const f32 row_y = ImGui::GetCursorPosY();

            if ( row < node->inputs.size ) {
                FrameGraphResource* input_resource = access_resource( node->inputs[ row ] );

                ImGui::SetCursorPos( ImVec2( content_start_x, row_y ) );
                const ed::PinId input_pin_id = frame_graph_input_pin_id( node_handle, row );
                const FrameGraphResourceHandle input_handle = node->inputs[ row ];
                if ( draw_frame_graph_pin( input_resource->name, input_pin_id, ed::PinKind::Input,
                                           debug_image_resource.index == input_handle.index ) ) {
                    select_frame_graph_debug_image( this, input_handle );
                }

                ed::PinId output_pin_id;
                if ( find_frame_graph_output_pin( this, node_handle, row, output_pin_id ) ) {
                    ed::Link( frame_graph_link_id( node_handle, row ), output_pin_id, input_pin_id );
                }
            }

            if ( row < node->outputs.size ) {
                FrameGraphResource* output_resource = builder->access_resource( node->outputs[ row ] );
                const f32 output_width = frame_graph_pin_width( output_resource->name );

                ImGui::SetCursorPos( ImVec2( content_start_x + content_width - output_width, row_y ) );
                const FrameGraphResourceHandle output_handle = node->outputs[ row ];
                if ( draw_frame_graph_pin( output_resource->name, frame_graph_output_pin_id( node_handle, row ), ed::PinKind::Output,
                                           debug_image_resource.index == output_handle.index ) ) {
                    select_frame_graph_debug_image( this, output_handle );
                }
            }

        }

        ed::EndNode();
    }
    ed::End();

    if ( layout_requested && nodes.size > 0 ) {
        ArenaAllocator* scratch_allocator = MemoryService::instance()->get_thread_allocator();
        ArenaScope scratch_scope( scratch_allocator );
        Array<FrameGraphLayoutScratchEntry> layout;
        const u32 layout_entry_count = max( node_layer.size, nodes.size + 1 );
        init_frame_graph_scratch_array( layout, scratch_allocator, layout_entry_count );
        u32 layer_count = 0;

        const ed::Style& editor_style = ed::GetStyle();
        // LinkStrength is intended for Bezier curvature and is unnecessarily
        // large when used verbatim as column spacing.
        const f32 horizontal_gap = max( editor_style.LinkStrength * 0.6f,
                                        ImGui::GetTextLineHeight() * 2.0f );
        const f32 vertical_gap = editor_style.NodePadding.y + editor_style.NodePadding.w + ImGui::GetStyle().ItemSpacing.y;
        // GetNodeSize() is reported in the current canvas' scaled coordinates,
        // while SetNodePosition() consumes editor-space coordinates.
        const f32 node_size_to_editor_space = ed::GetCurrentZoom();

        for ( u32 n = 0; n < nodes.size; ++n ) {
            const FrameGraphNodeHandle handle = nodes[ n ];
            const u32 layer = node_layer[ handle.index ];
            const ImVec2 measured_node_size = ed::GetNodeSize( frame_graph_node_id( handle ) );
            const ImVec2 node_size( measured_node_size.x * node_size_to_editor_space,
                                    measured_node_size.y * node_size_to_editor_space );

            layout[ handle.index ].node_width = node_size.x;
            layer_count = max( layer_count, layer + 1 );
            layout[ layer ].layer_width = max( layout[ layer ].layer_width, node_size.x );
            layout[ layer ].layer_height += node_size.y;
            ++layout[ layer ].nodes_in_layer;
            layout[ handle.index ].node_is_in_layout = true;
        }

        for ( u32 layer = 0; layer < layer_count; ++layer ) {
            layout[ layer + 1 ].layer_offset = layout[ layer ].layer_offset + layout[ layer ].nodes_in_layer;
            layout[ layer ].layer_write_offset = layout[ layer ].layer_offset;
        }

        // Group nodes by layer while retaining their topological order as the
        // deterministic initial ordering.
        for ( u32 n = 0; n < nodes.size; ++n ) {
            const FrameGraphNodeHandle handle = nodes[ n ];
            const u32 layer = node_layer[ handle.index ];
            layout[ layout[ layer ].layer_write_offset++ ].ordered_handle = handle;
        }

        for ( u32 layer = 0; layer < layer_count; ++layer ) {
            for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                layout[ layout[ i ].ordered_handle.index ].node_order = i - layout[ layer ].layer_offset;
            }
        }

        // Alternating barycentric sweeps reduce crossings. A forward sweep orders
        // consumers around their producers; the reverse sweep does the same for
        // producers around their consumers.
        for ( u32 iteration = 0; iteration < 4; ++iteration ) {
            for ( u32 layer = 1; layer < layer_count; ++layer ) {
                const u32 begin = layout[ layer ].layer_offset;
                const u32 end = layout[ layer + 1 ].layer_offset;

                for ( u32 i = begin; i < end; ++i ) {
                    FrameGraphNode* node = access_node( layout[ i ].ordered_handle );
                    f32 order_sum = 0.0f;
                    u32 producer_count = 0;

                    for ( u32 input_index = 0; input_index < node->inputs.size; ++input_index ) {
                        FrameGraphResource* input_resource = access_resource( node->inputs[ input_index ] );
                        const FrameGraphNodeHandle producer = input_resource->producer;

                        if ( producer.index != k_invalid_index && layout[ producer.index ].node_is_in_layout ) {
                            order_sum += layout[ producer.index ].node_order;
                            ++producer_count;
                        }
                    }

                    layout[ i ].barycenter = producer_count > 0 ? order_sum / producer_count : f32( i - begin );
                }

                sort_frame_graph_layout_layer( layout.data, begin, end );
                for ( u32 i = begin; i < end; ++i ) {
                    layout[ layout[ i ].ordered_handle.index ].node_order = i - begin;
                }
            }

            for ( i32 layer = i32( layer_count ) - 2; layer >= 0; --layer ) {
                const u32 begin = layout[ layer ].layer_offset;
                const u32 end = layout[ layer + 1 ].layer_offset;

                for ( u32 i = begin; i < end; ++i ) {
                    FrameGraphNode* node = access_node( layout[ i ].ordered_handle );
                    f32 order_sum = 0.0f;
                    u32 consumer_count = 0;

                    for ( u32 edge_index = 0; edge_index < node->edges.size; ++edge_index ) {
                        const FrameGraphNodeHandle consumer = node->edges[ edge_index ];
                        if ( consumer.index < FrameGraphBuilder::k_max_nodes_count && layout[ consumer.index ].node_is_in_layout ) {
                            order_sum += layout[ consumer.index ].node_order;
                            ++consumer_count;
                        }
                    }

                    layout[ i ].barycenter = consumer_count > 0 ? order_sum / consumer_count : f32( i - begin );
                }

                sort_frame_graph_layout_layer( layout.data, begin, end );
                for ( u32 i = begin; i < end; ++i ) {
                    layout[ layout[ i ].ordered_handle.index ].node_order = i - begin;
                }
            }
        }

        f32 graph_height = 0.0f;
        for ( u32 layer = 0; layer < layer_count; ++layer ) {
            if ( layout[ layer ].nodes_in_layer > 1 ) {
                layout[ layer ].layer_height += vertical_gap * ( layout[ layer ].nodes_in_layer - 1 );
            }
            graph_height = max( graph_height, layout[ layer ].layer_height );

            if ( layer > 0 ) {
                layout[ layer ].layer_x = layout[ layer - 1 ].layer_x + layout[ layer - 1 ].layer_width + horizontal_gap;
            }
        }

        for ( u32 layer = 0; layer < layer_count; ++layer ) {
            layout[ layer ].layer_y = ( graph_height - layout[ layer ].layer_height ) * 0.5f;

            for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                const FrameGraphNodeHandle handle = layout[ i ].ordered_handle;
                const ImVec2 measured_node_size = ed::GetNodeSize( frame_graph_node_id( handle ) );

                layout[ handle.index ].node_height = measured_node_size.y * node_size_to_editor_space;
                layout[ handle.index ].node_y = layout[ layer ].layer_y;
                layout[ layer ].layer_y += layout[ handle.index ].node_height + vertical_gap;
            }
        }

        // Translate complete layers toward the average centers of their connected
        // neighbours. Translating the whole layer preserves the collision-free
        // packing established above while keeping related passes visually close.
        for ( u32 iteration = 0; iteration < 4; ++iteration ) {
            for ( u32 layer = 1; layer < layer_count; ++layer ) {
                f32 offset_sum = 0.0f;
                u32 connection_count = 0;

                for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                    const FrameGraphNodeHandle handle = layout[ i ].ordered_handle;
                    FrameGraphNode* node = access_node( handle );
                    const f32 node_center = layout[ handle.index ].node_y + layout[ handle.index ].node_height * 0.5f;

                    for ( u32 input_index = 0; input_index < node->inputs.size; ++input_index ) {
                        FrameGraphResource* input_resource = access_resource( node->inputs[ input_index ] );
                        const FrameGraphNodeHandle producer = input_resource->producer;

                        if ( producer.index != k_invalid_index && layout[ producer.index ].node_is_in_layout ) {
                            const f32 producer_center = layout[ producer.index ].node_y + layout[ producer.index ].node_height * 0.5f;
                            offset_sum += producer_center - node_center;
                            ++connection_count;
                        }
                    }
                }

                if ( connection_count > 0 ) {
                    const f32 offset = offset_sum / connection_count;
                    for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                        layout[ layout[ i ].ordered_handle.index ].node_y += offset;
                    }
                }
            }

            for ( i32 layer = i32( layer_count ) - 2; layer >= 0; --layer ) {
                f32 offset_sum = 0.0f;
                u32 connection_count = 0;

                for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                    const FrameGraphNodeHandle handle = layout[ i ].ordered_handle;
                    FrameGraphNode* node = access_node( handle );
                    const f32 node_center = layout[ handle.index ].node_y + layout[ handle.index ].node_height * 0.5f;

                    for ( u32 edge_index = 0; edge_index < node->edges.size; ++edge_index ) {
                        const FrameGraphNodeHandle consumer = node->edges[ edge_index ];
                        if ( consumer.index < FrameGraphBuilder::k_max_nodes_count && layout[ consumer.index ].node_is_in_layout ) {
                            const f32 consumer_center = layout[ consumer.index ].node_y + layout[ consumer.index ].node_height * 0.5f;
                            offset_sum += consumer_center - node_center;
                            ++connection_count;
                        }
                    }
                }

                if ( connection_count > 0 ) {
                    const f32 offset = offset_sum / connection_count;
                    for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                        layout[ layout[ i ].ordered_handle.index ].node_y += offset;
                    }
                }
            }
        }

        // Direct links that skip one or more layers can otherwise pass through
        // the nodes in those intermediate layers. Approximate each Bezier by
        // the line between its pin rows, repel intersecting nodes from that
        // corridor, then repack every layer to retain node/node separation.
        const f32 pin_row_step = ( ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y ) * node_size_to_editor_space;
        const f32 first_pin_offset = ( editor_style.NodePadding.y + ImGui::GetTextLineHeightWithSpacing() +
                                       ImGui::GetTextLineHeight() * 0.5f ) * node_size_to_editor_space;
        const f32 link_clearance = max( ImGui::GetStyle().ItemSpacing.y * node_size_to_editor_space,
                                        k_frame_graph_pin_radius * node_size_to_editor_space );

        for ( u32 iteration = 0; iteration < 6; ++iteration ) {
            for ( u32 n = 0; n < nodes.size; ++n ) {
                layout[ nodes[ n ].index ].avoidance_offset = 0.0f;
            }
            bool found_intersection = false;

            for ( u32 consumer_index = 0; consumer_index < nodes.size; ++consumer_index ) {
                const FrameGraphNodeHandle consumer_handle = nodes[ consumer_index ];
                FrameGraphNode* consumer = access_node( consumer_handle );
                const u32 consumer_layer = node_layer[ consumer_handle.index ];

                for ( u32 input_index = 0; input_index < consumer->inputs.size; ++input_index ) {
                    FrameGraphResource* input_resource = access_resource( consumer->inputs[ input_index ] );
                    const FrameGraphNodeHandle producer_handle = input_resource->producer;

                    if ( producer_handle.index == k_invalid_index || !layout[ producer_handle.index ].node_is_in_layout ) {
                        continue;
                    }

                    const u32 producer_layer = node_layer[ producer_handle.index ];
                    if ( producer_layer + 1 >= consumer_layer ) {
                        continue;
                    }

                    FrameGraphNode* producer = access_node( producer_handle );
                    const FrameGraphResourceHandle source_handle = consumer->version == 2
                        ? consumer->inputs[ input_index ]
                        : input_resource->output_handle;
                    u32 output_index = 0;
                    for ( ; output_index < producer->outputs.size; ++output_index ) {
                        if ( producer->outputs[ output_index ].index == source_handle.index ) {
                            break;
                        }
                    }

                    if ( output_index == producer->outputs.size ) {
                        continue;
                    }

                    const f32 source_y = layout[ producer_handle.index ].node_y + first_pin_offset +
                                         output_index * pin_row_step;
                    const f32 target_y = layout[ consumer_handle.index ].node_y + first_pin_offset + input_index * pin_row_step;
                    const f32 source_x = layout[ producer_layer ].layer_x + layout[ producer_handle.index ].node_width;
                    const f32 target_x = layout[ consumer_layer ].layer_x;

                    for ( u32 layer = producer_layer + 1; layer < consumer_layer; ++layer ) {
                        const f32 sample_x = layout[ layer ].layer_x + layout[ layer ].layer_width * 0.5f;
                        const f32 linear_interpolation = target_x > source_x
                            ? ( sample_x - source_x ) / ( target_x - source_x )
                            : 0.5f;
                        const f32 clamped_interpolation = min( max( linear_interpolation, 0.0f ), 1.0f );
                        // Pin links have horizontal Bezier tangents, so their Y
                        // coordinate follows a smoothstep rather than a line.
                        const f32 interpolation = clamped_interpolation * clamped_interpolation *
                                                  ( 3.0f - 2.0f * clamped_interpolation );
                        const f32 link_y = source_y + ( target_y - source_y ) * interpolation;

                        for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                            const FrameGraphNodeHandle obstacle = layout[ i ].ordered_handle;
                            const f32 obstacle_top = layout[ obstacle.index ].node_y;
                            const f32 obstacle_bottom = obstacle_top + layout[ obstacle.index ].node_height;

                            if ( link_y + link_clearance <= obstacle_top ||
                                 link_y - link_clearance >= obstacle_bottom ) {
                                continue;
                            }

                            const f32 obstacle_center = ( obstacle_top + obstacle_bottom ) * 0.5f;
                            const f32 displacement = link_y < obstacle_center
                                ? link_y + link_clearance - obstacle_top
                                : link_y - link_clearance - obstacle_bottom;

                            if ( fabsf( displacement ) > fabsf( layout[ obstacle.index ].avoidance_offset ) ) {
                                layout[ obstacle.index ].avoidance_offset = displacement;
                            }
                            found_intersection = true;
                        }
                    }
                }
            }

            if ( !found_intersection ) {
                break;
            }

            for ( u32 n = 0; n < nodes.size; ++n ) {
                const FrameGraphNodeHandle handle = nodes[ n ];
                layout[ handle.index ].node_y += layout[ handle.index ].avoidance_offset;
            }

            // Resolve any collisions caused by the link repulsion while keeping
            // the crossing-minimized order within each layer.
            for ( u32 layer = 0; layer < layer_count; ++layer ) {
                for ( u32 i = layout[ layer ].layer_offset + 1; i < layout[ layer + 1 ].layer_offset; ++i ) {
                    const FrameGraphNodeHandle previous = layout[ i - 1 ].ordered_handle;
                    const FrameGraphNodeHandle current = layout[ i ].ordered_handle;
                    layout[ current.index ].node_y = max( layout[ current.index ].node_y,
                                                          layout[ previous.index ].node_y + layout[ previous.index ].node_height + vertical_gap );
                }

                if ( layout[ layer + 1 ].layer_offset > layout[ layer ].layer_offset ) {
                    for ( i32 i = i32( layout[ layer + 1 ].layer_offset ) - 2; i >= i32( layout[ layer ].layer_offset ); --i ) {
                        const FrameGraphNodeHandle current = layout[ i ].ordered_handle;
                        const FrameGraphNodeHandle next = layout[ i + 1 ].ordered_handle;
                        layout[ current.index ].node_y = min( layout[ current.index ].node_y,
                                                              layout[ next.index ].node_y - layout[ current.index ].node_height - vertical_gap );
                    }
                }
            }
        }

        for ( u32 layer = 0; layer < layer_count; ++layer ) {
            for ( u32 i = layout[ layer ].layer_offset; i < layout[ layer + 1 ].layer_offset; ++i ) {
                const FrameGraphNodeHandle handle = layout[ i ].ordered_handle;

                ed::SetNodePosition( frame_graph_node_id( handle ), ImVec2( layout[ layer ].layer_x, layout[ handle.index ].node_y ) );
            }
        }

        ed::NavigateToContent( 0.0f );
    }

    ed::SetCurrentEditor(nullptr);

}

bool FrameGraph::consume_debug_image_selection( ImageViewHandle& image_view ) {
    if ( !debug_image_selection_pending ) {
        return false;
    }

    debug_image_selection_pending = false;
    image_view = debug_image_view;
    return image_view.is_valid();
}

void FrameGraph::clear_debug_image_selection() {
    debug_image_resource.index = k_invalid_index;
    debug_image_view = {};
    debug_image_selection_pending = false;
}

void FrameGraph::add_node( FrameGraphNodeCreation& creation ) {
    FrameGraphNodeHandle handle = builder->create_node( creation );
    all_nodes.push( handle );
}

void FrameGraph::add_node_v2( const FrameGraphNodeCreation_v2& creation ) {
    FrameGraphNodeHandle handle = builder->create_node_v2( creation );
    all_nodes.push( handle );
}

FrameGraphNode* FrameGraph::get_node( cstring name ) {
    return builder->get_node( name );
}

FrameGraphNode* FrameGraph::access_node( FrameGraphNodeHandle handle ) {
    return builder->access_node( handle );
}

void FrameGraph::cache_render_pass_output( cstring node_name, GpuDevice* gpu, RenderPassOutput& render_pass_output, bool compute_node ) {
    // Compute pipelines have no attachments: they do not need a node.
    if ( compute_node ) {
        render_pass_output = gpu->get_swapchain_output();
        return;
    }

    FrameGraphNode* node = node_name ? get_node( node_name ) : nullptr;
    if ( node ) {

        // TODO: handle better
        if ( strcmp( node_name, "swapchain" ) == 0 ) {
            render_pass_output = gpu->get_swapchain_output();
        } else if ( compute_node ) {
            render_pass_output = gpu->get_swapchain_output();
        } else {
            render_pass_output = node->render_pass_output;
        }
    } else {
        rprint( "Cannot find render pass %s. Defaulting to swapchain\n", node_name );
        render_pass_output = gpu->get_swapchain_output();
    }
}

void FrameGraph::add_resource( cstring name, FrameGraphResourceType type, FrameGraphResourceInfo resource_info ) {
    builder->add_resource( name, type, resource_info );
}

FrameGraphResource* FrameGraph::get_resource( cstring name ) {
    return builder->get_resource( name );
}

FrameGraphResource* FrameGraph::access_resource( FrameGraphResourceHandle handle ) {
    return builder->access_resource( handle );
}

FrameGraphResource* FrameGraph::access_output_resource( FrameGraphResourceHandle handle ) {
    FrameGraphResource* resource = builder->access_resource( handle );
    RASSERT( resource != nullptr );
    if ( resource->output_handle.index != k_invalid_index ) {
        resource = builder->access_resource( resource->output_handle );
    }

    return resource;
}

// FrameGraphRenderPassCache /////////////////////////////////////////////////////////////

void FrameGraphRenderPassCache::init( Allocator* allocator )
{
    render_pass_map.init( allocator, FrameGraphBuilder::k_max_render_pass_count );
}

void FrameGraphRenderPassCache::shutdown( )
{
    render_pass_map.shutdown( );
}

// FrameGraphResourceCache /////////////////////////////////////////////////////////////

void FrameGraphResourceCache::init( Allocator* allocator, GpuDevice* device_ )
{
    device = device_;

    resources.init( allocator, FrameGraphBuilder::k_max_resources_count );
    resource_map.init( allocator, FrameGraphBuilder::k_max_resources_count );
}

void FrameGraphResourceCache::shutdown( )
{
    FlatHashMapIterator it = resource_map.iterator_begin();
    while ( it.is_valid() ) {

        u32 resource_index = resource_map.get( it );
        FrameGraphResource* resource = resources.get( resource_index );

        if ( ( resource->type == FrameGraphResourceType_Texture || resource->type == FrameGraphResourceType_Attachment ) ) {

            // Avoid external resources - must be handled by their owner
            if ( resource->resource_info.external ) {
                resource_map.iterator_advance( it );
                continue;
            }

            if ( resource->resource_info.texture.image.is_valid() ) {
                Image* image = device->get_image( resource->resource_info.texture.image );
                device->destroy_image( image->handle );
            }

            if ( resource->resource_info.texture.image_view.is_valid() ) {
                ImageView* view = device->get_image_view( resource->resource_info.texture.image_view );
                device->destroy_image_view( view->handle );
            }


            if ( resource->resource_info.texture.previous_image.is_valid() ) {
                Image* image = device->get_image( resource->resource_info.texture.previous_image );
                device->destroy_image( image->handle );
            }

            if ( resource->resource_info.texture.previous_image_view.is_valid() ) {
                ImageView* view = device->get_image_view( resource->resource_info.texture.previous_image_view );
                device->destroy_image_view( view->handle );
            }

        } else if ( ( resource->type == FrameGraphResourceType_Buffer )
                    && ( resource->resource_info.buffer.handle.is_valid() ) ) {
            Buffer* buffer = device->get_buffer( resource->resource_info.buffer.handle );
            device->destroy_buffer( buffer->handle );
        }

        resource_map.iterator_advance( it );
    }

    resources.free_all_resources();
    resources.shutdown();
    resource_map.shutdown( );
}

// FrameGraphNodeCache /////////////////////////////////////////////////////////////

void FrameGraphNodeCache::init( Allocator* allocator, GpuDevice* device_ )
{
    device = device_;

    nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count, sizeof( FrameGraphNode ) );
    node_map.init( allocator, FrameGraphBuilder::k_max_nodes_count );
}

void FrameGraphNodeCache::shutdown( )
{
    nodes.free_all_resources();
    nodes.shutdown( );
    node_map.shutdown();
}

// FrameGraphBuilder /////////////////////////////////////////////////////////////

void FrameGraphBuilder::init( GpuDevice* device_ ) {
    device = device_;

    allocator = device->allocator;

    resource_cache.init( allocator, device );
    node_cache.init( allocator, device );
    render_pass_cache.init( allocator );

    rename_buffer.init( rkilo( 16 ), allocator );
}

void FrameGraphBuilder::shutdown() {
    resource_cache.shutdown( );
    node_cache.shutdown( );
    render_pass_cache.shutdown( );

    rename_buffer.shutdown();
}

FrameGraphResourceHandle FrameGraphBuilder::create_node_output( const FrameGraphResourceCreation& creation, FrameGraphNodeHandle producer )
{
    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more node output (cannot allocate %s)!\n", creation.name );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );
    resource->name = creation.name;
    resource->type = creation.type;

    if ( creation.type != FrameGraphResourceType_Reference ) {
        resource->resource_info = creation.resource_info;
        resource->output_handle = resource_handle;
        resource->producer = producer;
        resource->ref_count = 0;

        if ( resource->type == FrameGraphResourceType_Buffer ) {
            resource->resource_info.buffer.handle = {};
        } else {
            resource->resource_info.texture.image = ImageHandle();
            resource->resource_info.texture.image_view = ImageViewHandle();
        }

        FrameGraphNode* producer_node = access_node( producer );
        RASSERT( producer_node != nullptr );

        if ( producer_node->enabled ) {
            // TODO(marco): eventually we want to allow enabling/disabling a node at runtime.
            // We will need to patch the producer when the graph changes
            resource_cache.resource_map.insert( hash_bytes( ( void* )resource->name, strlen( creation.name ) ), resource_handle.index );
        }
    }

    return resource_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::create_output_handle( const FrameGraphResourceCreation& creation )
{
    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more node output handle (cannot allocate %s)!\n", creation.name );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );
    resource->name = creation.name;
    resource->type = creation.type;

    RASSERT( creation.type != FrameGraphResourceType_Reference );
    resource->resource_info = creation.resource_info;
    resource->output_handle = resource_handle;
    resource->ref_count = 0;
    resource->rename_count = 0;

    if ( resource->type == FrameGraphResourceType_Buffer ) {
        resource->resource_info.buffer.handle = {};
    } else {
        resource->resource_info.texture.image = ImageHandle();
        resource->resource_info.texture.image_view = ImageViewHandle();
    }

    resource_cache.resource_map.insert( hash_bytes( ( void* )resource->name, strlen( creation.name ) ), resource_handle.index );

    return resource_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::create_output_reference( FrameGraphResourceHandle original_handle, FrameGraphResourceType type )
{
    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph output references!\n" );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );

    FrameGraphResource* original_resource = resource_cache.resources.get( original_handle.index );
    while ( original_resource->type == FrameGraphResourceType_Reference ) {
        original_handle = original_resource->output_handle;
        original_resource = resource_cache.resources.get( original_handle.index );
    }

    resource->name = rename_buffer.append_use_f( "%s(%u)", original_resource->name, ++original_resource->rename_count );
    resource->type = type;
    resource->output_handle = original_handle;

    return resource_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::create_node_input( const FrameGraphResourceCreation& creation )
{
    FrameGraphResourceHandle resource_handle = { k_invalid_index };

    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph node input (cannot allocate %s)!\n", creation.name );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );

    resource->resource_info = { };
    resource->producer.index = k_invalid_index;
    resource->output_handle.index = k_invalid_index;
    resource->type = creation.type;
    resource->name = creation.name;
    resource->ref_count = 0;

    return resource_handle;
}

FrameGraphNodeHandle FrameGraphBuilder::create_node( const FrameGraphNodeCreation& creation )
{
    FrameGraphNodeHandle node_handle{ k_invalid_index };
    node_handle.index = node_cache.nodes.obtain_resource();

    if ( node_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph nodes (cannot allocate %s)!\n", creation.name );
        return node_handle;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_handle.index );
    node->version = 1;
    node->name = creation.name;
    node->enabled = creation.enabled;
    node->compute = creation.compute;
    node->ray_tracing = creation.ray_tracing;
    node->inputs.init( allocator, ( u32 )creation.inputs.size );
    node->outputs.init( allocator, ( u32 )creation.outputs.size );
    node->edges.init( allocator, ( u32 )creation.outputs.size );
    node->scheduling = { CommandQueueType::Graphics, 0 };

    node_cache.node_map.insert( hash_bytes( ( void* )node->name, strlen( node->name ) ), node_handle.index );

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( u32 i = 0; i < creation.outputs.size; ++i ) {
        const FrameGraphResourceCreation& output_creation = creation.outputs[ i ];

        FrameGraphResourceHandle output = create_node_output( output_creation, node_handle );

        node->outputs.push( output );
    }

    for ( u32 i = 0; i < creation.inputs.size; ++i ) {
        const FrameGraphResourceCreation& input_creation = creation.inputs[ i ];

        FrameGraphResourceHandle input_handle = create_node_input( input_creation );

        node->inputs.push( input_handle );
    }

    return node_handle;
}

FrameGraphNodeHandle FrameGraphBuilder::create_node_v2( const FrameGraphNodeCreation_v2& creation )
{
    FrameGraphNodeHandle node_handle{ k_invalid_index };
    node_handle.index = node_cache.nodes.obtain_resource();

    if ( node_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph nodes (cannot allocate %s)!\n", creation.name );
        return node_handle;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_handle.index );
    node->version = 2;
    node->name = creation.name;
    node->enabled = creation.enabled;
    node->compute = creation.compute;
    node->ray_tracing = creation.ray_tracing;
    node->inputs.init( allocator, ( u32 )creation.inputs.size );
    node->temp_inputs_type.init( allocator, ( u32 )creation.inputs.size );
    node->outputs.init( allocator, ( u32 )creation.outputs.size );
    node->edges.init( allocator, ( u32 )creation.outputs.size );
    node->scheduling = creation.scheduling;

    node_cache.node_map.insert( hash_bytes( ( void* )node->name, strlen( node->name ) ), node_handle.index );

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( u32 i = 0; i < creation.outputs.size; ++i ) {
        FrameGraphResourceHandle output = creation.outputs[ i ];
        FrameGraphResource* resource = access_resource( output );
        RASSERT( resource != nullptr );

        resource->producer = node_handle;

        node->outputs.push( output );
    }

    for ( u32 i = 0; i < creation.inputs.size; ++i ) {
        node->inputs.push( creation.inputs[ i ].handle );
        node->temp_inputs_type.push( creation.inputs[ i ].type );

        // Safety check for input/outputs problems, normally due to Span problems.
        const FrameGraphResourceHandle input = creation.inputs[ i ].handle;
        for ( u32 o = 0; o < creation.outputs.size; ++o ) {
            RASSERTM( input.index != creation.outputs[ o ].index, "Node %s uses resource handle %u as both input and output", creation.name, input.index );
        }
    }

    return node_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::get_output_handle( cstring node_name, cstring resource_name ) {
    FrameGraphNode* node = get_node( node_name );
    if ( node == nullptr ) {
        rprint( "Cannot find node %s in frame graph!\n", node_name );
        return { k_invalid_index };
    }

    for ( u32 i = 0; i < node->outputs.size; ++i ) {
        FrameGraphResource* resource = access_resource( node->outputs[ i ] );

        // NOTE(marco): we use strstr here to allow for renaming of resources
        if ( strstr( resource->name, resource_name ) != nullptr ) {
            return node->outputs[ i ];
        }
    }

    return { k_invalid_index };
}

FrameGraphNode* FrameGraphBuilder::get_node( cstring name ) {
    FlatHashMapIterator it = node_cache.node_map.find( hash_calculate( name ) );
    if ( it.is_invalid() ) {
        rprint( "Cannot find node %s in frame graph!\n", name );
        return nullptr;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_cache.node_map.get( it ) );

    return node;
}

FrameGraphNode* FrameGraphBuilder::access_node( FrameGraphNodeHandle handle ) {
    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( handle.index );

    return node;
}

void FrameGraphBuilder::add_resource( cstring name, FrameGraphResourceType type, FrameGraphResourceInfo resource_info ) {
    FlatHashMapIterator it = resource_cache.resource_map.find( hash_calculate( name ) );
    assert( it.is_invalid() );

    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph resources (cannot allocate %s)!\n", name );
        return;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );
    resource->name = name;
    resource->type = type;

    resource->resource_info = resource_info;
    resource->ref_count = 0;

    resource_cache.resource_map.insert( hash_bytes( ( void* )name, strlen( name ) ), resource_handle.index );
}

FrameGraphResource* FrameGraphBuilder::get_resource( cstring name ) {
    FlatHashMapIterator it = resource_cache.resource_map.find( hash_calculate( name ) );
    if ( it.is_invalid() ) {
        //rprint( "Cannot find resource %s in frame graph!\n", name );
        return nullptr;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_cache.resource_map.get( it ) );

    return resource;
}

FrameGraphResource* FrameGraphBuilder::access_resource( FrameGraphResourceHandle handle ) {
    FrameGraphResource* resource = resource_cache.resources.get( handle.index );

    return resource;
}

void FrameGraphBuilder::register_render_pass( cstring name, FrameGraphRenderPass* render_pass )
{
    u64 key = hash_calculate( name );

    FlatHashMapIterator it = render_pass_cache.render_pass_map.find( key );
    if ( it.is_valid() ) {
        rprint( "Render pass %s already registered in frame graph!\n", name );
        return;
    }

    it = node_cache.node_map.find( key );
    if ( it.is_invalid() ) {
        rprint( "Cannot find node %s to register render pass!\n", name );
        return;
    }

    render_pass_cache.render_pass_map.insert( key, render_pass );

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_cache.node_map.get( it ) );
    node->graph_render_pass = render_pass;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_external( bool value ) {
    external = value;
    return *this;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_buffer( sizet size, VkBufferUsageFlags flags, BufferHandle handle ) {
    buffer.size = size;
    buffer.flags = flags;
    buffer.handle = handle;
    return *this;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_external_texture_2d( u32 width, u32 height, VkFormat format, VkImageUsageFlags flags, ImageHandle image, ImageViewHandle image_view ) {

    texture.width = width;
    texture.height = height;
    texture.depth = 1;
    texture.format = format;
    texture.flags = flags;
    texture.image = image;
    texture.image_view = image_view;

    external = true;

    return *this;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_external_texture_3d( u32 width, u32 height, u32 depth, VkFormat format, VkImageUsageFlags flags, ImageHandle image, ImageViewHandle image_view ) {

    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.format = format;
    texture.flags = flags;
    texture.image = image;
    texture.image_view = image_view;

    external = true;

    return *this;
}

} // namespace raptor
