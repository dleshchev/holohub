/*
 * Copyright (c) 2022, DELTACAST.TV.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "videomaster_source.hpp"

#include <cuda.h>
#include <cuda_runtime.h>

#include <string>
#include <utility>

#include "VideoMasterHD_ApplicationBuffers.h"
#include "gxf/multimedia/video.hpp"

namespace nvidia {
namespace holoscan {
namespace videomaster {

VideoMasterSource::VideoMasterSource() : VideoMasterBase(true) {}

gxf_result_t VideoMasterSource::registerInterface(gxf::Registrar *registrar) {
  gxf::Expected<void> result;
  result &= registrar->parameter(_use_rdma, "use_rdma", "Use RDMA", "Specifies whether RDMA should be used.");
  result &= registrar->parameter(_board_index, "board", "Board", "Index of the Deltacast.TV board to use.");
  result &= registrar->parameter(_channel_index, "input", "Input", "Index of the input channel to use.");
  result &= registrar->parameter(_signal, "signal", "Output", "Output signal.");
  result &= registrar->parameter(_pool, "pool", "Pool", "Pool to allocate the buffers.");

  return gxf::ToResultCode(result);
}

gxf_result_t VideoMasterSource::start() {
  gxf::Expected<void> result;
  result &= configure_board();
  result &= open_stream();

  return gxf::ToResultCode(result);
}

gxf_result_t VideoMasterSource::tick() {
  if (!signal_present()) {
    if (!_has_lost_signal)
      GXF_LOG_INFO("No signal detected, waiting for input...");
    _has_lost_signal = true;
    return GXF_SUCCESS;
  }

  if (_has_lost_signal) {
    GXF_LOG_INFO("Input signal detected");
    _has_lost_signal = false;
  }

  auto input_information = get_input_information();
  if (input_information != _video_information->stream_properties_values) {
    _video_information->stream_properties_values = input_information;

    GXF_LOG_INFO("Input signal has changed, restarting stream");
    VHD_StopStream(_stream_handle);

    gxf::Expected<void> result;
    result &= configure_stream();
    result &= init_buffers();
    result &= start_stream();

    if (!result)
      return gxf::ToResultCode(result);
  }

  HANDLE slot_handle;
  ULONG api_result = VHD_WaitSlotFilled(_stream_handle, &slot_handle, SLOT_TIMEOUT);
  if (api_result != VHDERR_NOERROR && api_result != VHDERR_TIMEOUT) {
    GXF_LOG_ERROR("Failed to wait for incoming slot");
    return GXF_FAILURE;
  }
  if (api_result != VHDERR_NOERROR && api_result == VHDERR_TIMEOUT) {
    GXF_LOG_INFO("Timeout");
    return GXF_SUCCESS;
  }

  BYTE *buffer = nullptr;
  ULONG buffer_size = 0;
  if (!api_call_success(VHD_GetSlotBuffer(slot_handle, _video_information->get_buffer_type(), &buffer, &buffer_size), "Failed to get slot buffer"))
    return GXF_FAILURE;

  auto result = transmit_buffer_data(buffer, buffer_size);

  VHD_QueueInSlot(slot_handle);
  _slot_count++;

  return gxf::ToResultCode(result);
}

gxf::Expected<void> VideoMasterSource::transmit_buffer_data(void *buffer, uint32_t buffer_size) {
  if (!_use_rdma) {
    cudaMemcpy(_rdma_buffers[_slot_count % NB_SLOTS][_video_information->get_buffer_type()].pointer(), buffer,
               buffer_size, cudaMemcpyHostToDevice);
    buffer = _rdma_buffers[_slot_count % NB_SLOTS][_video_information->get_buffer_type()].pointer();
  }
  auto message = gxf::Entity::New(context());
  if (!message) {
    GXF_LOG_ERROR("Failed to allocate message; terminating.");
    return gxf::Unexpected{GXF_FAILURE};
  }

  auto target_buffer = message.value().add<gxf::VideoBuffer>();
  if (!target_buffer) {
    GXF_LOG_ERROR("Failed to allocate video buffer; terminating.");
    return gxf::Unexpected{GXF_FAILURE};
  }

  auto format = _video_information->get_video_format();
  if (!format)
    return gxf::Unexpected{GXF_FAILURE};

  gxf::VideoTypeTraits<gxf::VideoFormat::GXF_VIDEO_FORMAT_RGBA> video_type;
  gxf::VideoFormatSize<gxf::VideoFormat::GXF_VIDEO_FORMAT_RGBA> color_format;
  auto color_planes = color_format.getDefaultColorPlanes(format->width, format->height);
  gxf::VideoBufferInfo info{format->width, format->height, video_type.value, color_planes,
                            gxf::SurfaceLayout::GXF_SURFACE_LAYOUT_PITCH_LINEAR};
  auto storage_type = gxf::MemoryStorageType::kDevice;
  target_buffer.value()->wrapMemory(info, buffer_size, storage_type, buffer, nullptr);

  return _signal->publish(std::move(message.value()));
}

}  // namespace videomaster
}  // namespace holoscan
}  // namespace nvidia
