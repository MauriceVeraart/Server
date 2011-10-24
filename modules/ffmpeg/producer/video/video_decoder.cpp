/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/
#include "../../stdafx.h"

#include "video_decoder.h"

#include "../util.h"
#include "../filter/filter.h"

#include "../../ffmpeg_error.h"

#include <core/producer/frame/basic_frame.h>
#include <common/memory/memcpy.h>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include <tbb/scalable_allocator.h>

#undef Yield
using namespace Concurrency;

namespace caspar { namespace ffmpeg {
	
struct video_decoder::implementation : public Concurrency::agent, boost::noncopyable
{	
	int										index_;
	std::shared_ptr<AVCodecContext>			codec_context_;
	
	double									fps_;
	int64_t									nb_frames_;

	size_t									width_;
	size_t									height_;
	bool									is_progressive_;
	
	unbounded_buffer<video_decoder::source_element_t>	source_;
	ITarget<video_decoder::target_element_t>&			target_;
	
public:
	explicit implementation(video_decoder::source_t& source, video_decoder::target_t& target, AVFormatContext& context) 
		: codec_context_(open_codec(context, AVMEDIA_TYPE_VIDEO, index_))
		, fps_(static_cast<double>(codec_context_->time_base.den) / static_cast<double>(codec_context_->time_base.num))
		, nb_frames_(context.streams[index_]->nb_frames)
		, width_(codec_context_->width)
		, height_(codec_context_->height)
		, is_progressive_(true)
		, source_([this](const video_decoder::source_element_t& element){return element.first->stream_index == index_;})
		, target_(target)
	{		
		CASPAR_LOG(debug) << "[video_decoder] " << context.streams[index_]->codec->codec->long_name;
		
		CASPAR_VERIFY(width_ > 0, ffmpeg_error());
		CASPAR_VERIFY(height_ > 0, ffmpeg_error());

		source.link_target(&source_);

		start();
	}

	~implementation()
	{
		agent::wait(this);
	}
	
	std::shared_ptr<AVFrame> decode(AVPacket& packet)
	{
		std::shared_ptr<AVFrame> decoded_frame(avcodec_alloc_frame(), av_free);

		int frame_finished = 0;
		THROW_ON_ERROR2(avcodec_decode_video2(codec_context_.get(), decoded_frame.get(), &frame_finished, &packet), "[video_decocer]");

		// 1 packet <=> 1 frame.
		// If a decoder consumes less then the whole packet then something is wrong
		// that might be just harmless padding at the end, or a problem with the
		// AVParser or demuxer which puted more then one frame in a AVPacket.

		if(frame_finished == 0)	
			return nullptr;
				
		if(decoded_frame->repeat_pict > 0)
			CASPAR_LOG(warning) << "[video_decoder]: Field repeat_pict not implemented.";

		return decoded_frame;
	}

	virtual void run()
	{
		try
		{
			while(true)
			{
				auto element = receive(source_);
				auto packet = element.first;
			
				if(packet == loop_packet(index_))
				{					
					if(codec_context_->codec->capabilities & CODEC_CAP_DELAY)
					{
						AVPacket pkt;
						av_init_packet(&pkt);
						pkt.data = nullptr;
						pkt.size = 0;

						for(auto decoded_frame = decode(pkt); decoded_frame; decoded_frame = decode(pkt))
						{
							send(target_, target_element_t(dup_frame(make_safe_ptr(decoded_frame)), element.second));
							Context::Yield();
						}
					}

					avcodec_flush_buffers(codec_context_.get());
					send(target_, target_element_t(loop_video(), ticket_t()));
					continue;
				}

				if(packet == eof_packet(index_))
					break;

				auto decoded_frame = decode(*packet);
				if(!decoded_frame)
					continue;
		
				is_progressive_ = decoded_frame->interlaced_frame == 0;
				
				// C-TODO: Avoid duplication.
				// Need to dupliace frame data since avcodec_decode_video2 reuses it.
				send(target_, target_element_t(dup_frame(make_safe_ptr(decoded_frame)), element.second));				
				Context::Yield();
			}
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
		}
		
		send(target_, target_element_t(eof_video(), ticket_t()));

		done();
	}

	safe_ptr<AVFrame> dup_frame(const safe_ptr<AVFrame>& frame)
	{
		auto desc = get_pixel_format_desc(static_cast<PixelFormat>(frame->format), frame->width, frame->height);

		auto count = desc.planes.size();
		std::array<uint8_t*, 4> org_ptrs;
		std::array<uint8_t*, 4> real_ptrs; // We need to store the "real" pointers, due to alignment hack.
		parallel_for<size_t>(0, count, [&](size_t n)
		{
			auto size		= frame->linesize[n]*desc.planes[n].height;
			org_ptrs[n]		= frame->data[n];
			real_ptrs[n]	= reinterpret_cast<uint8_t*>(scalable_aligned_malloc(size+16, 32)); // Allocate 16 byte extra for alignment hack.
			frame->data[n]	= reinterpret_cast<uint8_t*>(fast_memcpy_w_align_hack(real_ptrs[n], org_ptrs[n], size));
		});

		return safe_ptr<AVFrame>(frame.get(), [frame, org_ptrs, real_ptrs, count](AVFrame*)
		{
			for(size_t n = 0; n < count; ++n)
			{
				scalable_aligned_free(real_ptrs[n]);
				frame->data[n] = org_ptrs[n];
			}
		});
	}
		
	double fps() const
	{
		return fps_;
	}
};

video_decoder::video_decoder(video_decoder::source_t& source, video_decoder::target_t& target, AVFormatContext& context) 
	: impl_(new implementation(source, target, context))
{
}

double video_decoder::fps() const{return impl_->fps();}
int64_t video_decoder::nb_frames() const{return impl_->nb_frames_;}
size_t video_decoder::width() const{return impl_->width_;}
size_t video_decoder::height() const{return impl_->height_;}
bool video_decoder::is_progressive() const{return impl_->is_progressive_;}

}}