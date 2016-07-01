// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#include "device.h"
#include "sync.h"
#include "motion_module.h"

#include <array>
#include "image.h"

#include <algorithm>
#include <sstream>
#include <iostream>

using namespace rsimpl;
using namespace rsimpl::motion_module;

rs_device_base::rs_device_base(std::shared_ptr<rsimpl::uvc::device> device, const rsimpl::static_device_info & info) : device(device), config(info), capturing(false), data_acquisition_active(false),
    depth(config, RS_STREAM_DEPTH), color(config, RS_STREAM_COLOR), infrared(config, RS_STREAM_INFRARED), infrared2(config, RS_STREAM_INFRARED2), fisheye(config, RS_STREAM_FISHEYE),
    points(depth), rect_color(color), color_to_depth(color, depth), depth_to_color(depth, color), depth_to_rect_color(depth, rect_color), infrared2_to_depth(infrared2,depth), depth_to_infrared2(depth,infrared2)
{
    streams[RS_STREAM_DEPTH    ] = native_streams[RS_STREAM_DEPTH]     = &depth;
    streams[RS_STREAM_COLOR    ] = native_streams[RS_STREAM_COLOR]     = &color;
    streams[RS_STREAM_INFRARED ] = native_streams[RS_STREAM_INFRARED]  = &infrared;
    streams[RS_STREAM_INFRARED2] = native_streams[RS_STREAM_INFRARED2] = &infrared2;
    streams[RS_STREAM_FISHEYE  ] = native_streams[RS_STREAM_FISHEYE]   = &fisheye;
    streams[RS_STREAM_POINTS]                                          = &points;
    streams[RS_STREAM_RECTIFIED_COLOR]                                 = &rect_color;
    streams[RS_STREAM_COLOR_ALIGNED_TO_DEPTH]                          = &color_to_depth;
    streams[RS_STREAM_DEPTH_ALIGNED_TO_COLOR]                          = &depth_to_color;
    streams[RS_STREAM_DEPTH_ALIGNED_TO_RECTIFIED_COLOR]                = &depth_to_rect_color;
    streams[RS_STREAM_INFRARED2_ALIGNED_TO_DEPTH]                      = &infrared2_to_depth;
    streams[RS_STREAM_DEPTH_ALIGNED_TO_INFRARED2]                      = &depth_to_infrared2;
}

rs_device_base::~rs_device_base()
{
    try
    {
        if (capturing) 
            stop(RS_SOURCE_VIDEO);
        if (data_acquisition_active)
            stop(RS_SOURCE_MOTION_TRACKING);
    }
    catch (...) {}
}

bool rs_device_base::supports_option(rs_option option) const 
{ 
    if(uvc::is_pu_control(option)) return true;
    for(auto & o : config.info.options) if(o.option == option) return true;
    return false; 
}

void rs_device_base::enable_stream(rs_stream stream, int width, int height, rs_format format, int fps, rs_output_buffer_format output)
{
    if(capturing) throw std::runtime_error("streams cannot be reconfigured after having called rs_start_device()");
    if(config.info.stream_subdevices[stream] == -1) throw std::runtime_error("unsupported stream");

    config.requests[stream] = { true, width, height, format, fps, output };
    for(auto & s : native_streams) s->archive.reset(); // Changing stream configuration invalidates the current stream info
}

void rs_device_base::enable_stream_preset(rs_stream stream, rs_preset preset)
{
    if(capturing) throw std::runtime_error("streams cannot be reconfigured after having called rs_start_device()");
    if(!config.info.presets[stream][preset].enabled) throw std::runtime_error("unsupported stream");

    config.requests[stream] = config.info.presets[stream][preset];
    for(auto & s : native_streams) s->archive.reset(); // Changing stream configuration invalidates the current stream info
}

void rs_device_base::disable_stream(rs_stream stream)
{
    if(capturing) throw std::runtime_error("streams cannot be reconfigured after having called rs_start_device()");
    if(config.info.stream_subdevices[stream] == -1) throw std::runtime_error("unsupported stream");

    config.requests[stream] = {};
    for(auto & s : native_streams) s->archive.reset(); // Changing stream configuration invalidates the current stream info
}

void rs_device_base::set_stream_callback(rs_stream stream, void (*on_frame)(rs_device * device, rs_frame_ref * frame, void * user), void * user)
{
    config.callbacks[stream] = frame_callback_ptr(new frame_callback{ this, on_frame, user });
}

void rs_device_base::set_stream_callback(rs_stream stream, rs_frame_callback* callback)
{
    config.callbacks[stream] = frame_callback_ptr(callback);
}

void rs_device_base::enable_motion_tracking()
{
    if (data_acquisition_active) throw std::runtime_error("motion-tracking cannot be reconfigured after having called rs_start_device()");

    config.data_requests.enabled = true;
}

void rs_device_base::disable_motion_tracking()
{
    if (data_acquisition_active) throw std::runtime_error("motion-tracking disabled after having called rs_start_device()");

    config.data_requests.enabled = false;
}

void rs_device_base::set_motion_callback(rs_motion_callback* callback)
{
    if (data_acquisition_active) throw std::runtime_error("cannot set motion callback when motion data is active");
    
    // replace previous, if needed
    config.motion_callback = motion_callback_ptr(callback, [](rs_motion_callback* c) { c->release(); });
}

void rs_device_base::start_motion_tracking()
{
    if (data_acquisition_active) throw std::runtime_error("cannot restart data acquisition without stopping first");

    motion_module_parser parser;

    // Activate data polling handler
    if (config.data_requests.enabled)
    {
        // TODO -replace hard-coded value 3 which stands for fisheye subdevice   
        set_subdevice_data_channel_handler(*device, 3,
            [this, parser](const unsigned char * data, const int size) mutable
        {
            // Parse motion data
            auto events = parser(data, size);

            // Handle events by user-provided handlers
            for (auto & entry : events)
            {
                // Handle Motion data packets
                if (config.motion_callback)
                for (int i = 0; i < entry.imu_entries_num; i++)
                        config.motion_callback->on_event(entry.imu_packets[i]);
                
                // Handle Timestamp packets
                if (config.timestamp_callback)
                {
                    for (int i = 0; i < entry.non_imu_entries_num; i++)
                    {
                        auto tse = entry.non_imu_packets[i];
                        if (archive)
                            archive->on_timestamp(tse);
                        config.timestamp_callback->on_event(entry.non_imu_packets[i]);
                    }
                }
            }
        });
    }

    start_data_acquisition(*device);     // activate polling thread in the backend
    data_acquisition_active = true;
}

void rs_device_base::stop_motion_tracking()
{
    if (!data_acquisition_active) throw std::runtime_error("cannot stop data acquisition - is already stopped");
    stop_data_acquisition(*device);
    data_acquisition_active = false;
}

void rs_device_base::set_motion_callback(void(*on_event)(rs_device * device, rs_motion_data data, void * user), void * user)
{
    if (data_acquisition_active) throw std::runtime_error("cannot set motion callback when motion data is active");
    
    // replace previous, if needed
    config.motion_callback = motion_callback_ptr(new motion_events_callback(this, on_event, user), [](rs_motion_callback* c) { delete c; });
}

void rs_device_base::set_timestamp_callback(void(*on_event)(rs_device * device, rs_timestamp_data data, void * user), void * user)
{
    if (data_acquisition_active) throw std::runtime_error("cannot set timestamp callback when motion data is active");

    config.timestamp_callback = timestamp_callback_ptr(new timestamp_events_callback{ this, on_event, user }, [](rs_timestamp_callback* c) { delete c; });
}

void rs_device_base::set_timestamp_callback(rs_timestamp_callback* callback)
{
    // replace previous, if needed
    config.timestamp_callback = timestamp_callback_ptr(callback, [](rs_timestamp_callback* c) { c->release(); });
}

void rs_device_base::start(rs_source source)
{
    if (source & RS_SOURCE_VIDEO)
        start_video_streaming();

    if (source & RS_SOURCE_MOTION_TRACKING)
        start_motion_tracking();
}

void rs_device_base::stop(rs_source source)
{
    if (source & RS_SOURCE_VIDEO)
        stop_video_streaming();

    if (source & RS_SOURCE_MOTION_TRACKING)
        stop_motion_tracking();
}

void rs_device_base::start_video_streaming()
{
    if(capturing) throw std::runtime_error("cannot restart device without first stopping device");
        
    auto selected_modes = config.select_modes();
    auto archive = std::make_shared<syncronizing_archive>(selected_modes, select_key_stream(selected_modes));
    auto timestamp_reader = create_frame_timestamp_reader();

    for(auto & s : native_streams) s->archive.reset(); // Starting capture invalidates the current stream info, if any exists from previous capture

    // Satisfy stream_requests as necessary for each subdevice, calling set_mode and
    // dispatching the uvc configuration for a requested stream to the hardware
    for(auto mode_selection : selected_modes)
    {
        // Create a stream buffer for each stream served by this subdevice mode
        for(auto & stream_mode : mode_selection.get_outputs())
        {                    
            // If this is one of the streams requested by the user, store the buffer so they can access it
            if(config.requests[stream_mode.first].enabled) native_streams[stream_mode.first]->archive = archive;
        }

        // Copy the callbacks that apply to this stream, so that they can be captured by value
        std::vector<rs_stream> streams;
        for (auto & output : mode_selection.get_outputs())
        {
            streams.push_back(output.first);
        }
        // Initialize the subdevice and set it to the selected mode
        set_subdevice_mode(*device, mode_selection.mode.subdevice, mode_selection.mode.native_dims.x, mode_selection.mode.native_dims.y, mode_selection.mode.pf.fourcc, mode_selection.mode.fps, 
        [this, mode_selection, archive, timestamp_reader, streams](const void * frame, std::function<void()> continuation) mutable
        {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto sys_time = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

            frame_continuation release_and_enqueue(continuation, frame);

            // Ignore any frames which appear corrupted or invalid
            if (!timestamp_reader->validate_frame(mode_selection.mode, frame)) return;

            // Determine the timestamp for this frame
            auto timestamp = timestamp_reader->get_frame_timestamp(mode_selection.mode, frame);
            auto frame_counter = timestamp_reader->get_frame_counter(mode_selection.mode, frame);
            
            auto requires_processing = mode_selection.requires_processing();

            auto width = mode_selection.get_unpacked_width();
            auto height = mode_selection.get_unpacked_height();
            auto fps = mode_selection.get_framerate();
            std::vector<byte *> dest;

            auto stride = mode_selection.get_stride();
            archive->correct_timestamp();

            for (auto & output : mode_selection.get_outputs())
            {
                auto bpp = rsimpl::get_image_bpp(output.second);
                frame_archive::frame_additional_data additional_data( timestamp,
                    frame_counter,
                    sys_time,
                    width,
                    height,
                    fps,
                    stride,
                    bpp,
                    output.second,
					output.first,
                    mode_selection.pad_crop);

                // Obtain buffers for unpacking the frame
                dest.push_back(archive->alloc_frame(output.first, additional_data, requires_processing));
            }
            // Unpack the frame
            if (requires_processing)
            {
                mode_selection.unpack(dest.data(), reinterpret_cast<const byte *>(frame));
            }

            // If any frame callbacks were specified, dispatch them now
            for (size_t i = 0; i < dest.size(); ++i)
            {
                if (!requires_processing)
                {
                    archive->attach_continuation(streams[i], std::move(release_and_enqueue));
                }

                if (config.callbacks[streams[i]])
                {
                    auto frame_ref = archive->track_frame(streams[i]);
                    if (frame_ref)
                    {
                        (*config.callbacks[streams[i]])->on_frame(this, (rs_frame_ref*)frame_ref);
                    }
                }
                else
                {
                    // Commit the frame to the archive
                    archive->commit_frame(streams[i]);
                }
            }
        });
    }
    
    this->archive = archive;
    on_before_start(selected_modes);
    start_streaming(*device, config.info.num_libuvc_transfer_buffers);
    capture_started = std::chrono::high_resolution_clock::now();
    capturing = true;
}

void rs_device_base::stop_video_streaming()
{
    if(!capturing) throw std::runtime_error("cannot stop device without first starting device");
    stop_streaming(*device);
    archive->flush();
    capturing = false;
}

void rs_device_base::wait_all_streams()
{
    if(!capturing) return;
    if(!archive) return;

    archive->wait_for_frames();
}

bool rs_device_base::poll_all_streams()
{
    if(!capturing) return false;
    if(!archive) return false;
    return archive->poll_for_frames();
}

rs_frameset* rs_device_base::wait_all_streams_safe()
{
    if (!capturing) throw std::runtime_error("Can't call wait_for_frames_safe when the device is not capturing!");
    if (!archive) throw std::runtime_error("Can't call wait_for_frames_safe when frame archive is not available!");

        return (rs_frameset*)archive->wait_for_frames_safe();
}

bool rs_device_base::poll_all_streams_safe(rs_frameset** frames)
{
    if (!capturing) return false;
    if (!archive) return false;

    return archive->poll_for_frames_safe((frame_archive::frameset**)frames);
}

void rs_device_base::release_frames(rs_frameset * frameset)
{
    archive->release_frameset((frame_archive::frameset *)frameset);
}

rs_frameset * rs_device_base::clone_frames(rs_frameset * frameset)
{
    auto result = archive->clone_frameset((frame_archive::frameset *)frameset);
    if (!result) throw std::runtime_error("Not enough resources to clone frameset!");
    return (rs_frameset*)result;
}


rs_frame_ref* rs_device_base::detach_frame(const rs_frameset* fs, rs_stream stream)
{
    auto result = archive->detach_frame_ref((frame_archive::frameset *)fs, stream);
    if (!result) throw std::runtime_error("Not enough resources to tack detached frame!");
    return (rs_frame_ref*)result;
}

void rs_device_base::release_frame(rs_frame_ref* ref)
{
    archive->release_frame_ref((frame_archive::frame_ref *)ref);
}

rs_frame_ref* ::rs_device_base::clone_frame(rs_frame_ref* frame)
{
    auto result = archive->clone_frame((frame_archive::frame_ref *)frame);
    if (!result) throw std::runtime_error("Not enough resources to clone frame!");
    return (rs_frame_ref*)result;
}

bool rs_device_base::supports(rs_capabilities capability) const
{
    for (auto elem: config.info.capabilities_vector)
    {
        if (elem == capability)
            return true;
    }

    return false;
}

void rs_device_base::get_option_range(rs_option option, double & min, double & max, double & step, double & def)
{
    if(uvc::is_pu_control(option))
    {
        int mn, mx, stp, df;
        uvc::get_pu_control_range(get_device(), config.info.stream_subdevices[RS_STREAM_COLOR], option, &mn, &mx, &stp, &df);
        min  = mn;
        max  = mx;
        step = stp;
        def  = df;
        return;
    }

    for(auto & o : config.info.options)
    {
        if(o.option == option)
        {
            min = o.min;
            max = o.max;
            step = o.step;
            def = o.def;
            return;
        }
    }

    throw std::logic_error("range not specified");
}

const char * rs_device_base::get_usb_port_id() const
{
    return rsimpl::uvc::get_usb_port_id(*device);
}
