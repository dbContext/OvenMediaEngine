//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "transcode_codec_dec_aac.h"

#include "../transcode_private.h"
#include "base/info/application.h"

void OvenCodecImplAvcodecDecAAC::ThreadDecode()
{
	while (!_kill_flag)
	{
		/////////////////////////////////////////////////////////////////////
		// Sending a packet to decoder
		/////////////////////////////////////////////////////////////////////
		if (_cur_pkt == nullptr && _input_buffer.IsEmpty() == false)
		{
			auto obj = _input_buffer.Dequeue();
			if (obj.has_value() == false)
			{
				continue;
			}

			_cur_pkt = std::move(obj.value());
			if (_cur_pkt != nullptr)
			{
				_cur_data = _cur_pkt->GetData();
				_pkt_offset = 0;
			}

			if ((_cur_data == nullptr) || (_cur_data->GetLength() == 0))
			{
				continue;
			}
		}

		if (_cur_data != nullptr)
		{
			while (_cur_data->GetLength() > _pkt_offset)
			{
				int32_t parsed_size = ::av_parser_parse2(
					_parser,
					_context,
					&_pkt->data, &_pkt->size,
					_cur_data->GetDataAs<uint8_t>() + _pkt_offset,
					static_cast<int32_t>(_cur_data->GetLength() - _pkt_offset),
					_cur_pkt->GetPts(), _cur_pkt->GetPts(),
					0);

				// Failed to parsing
				if (parsed_size <= 0)
				{
					logte("Error while parsing\n");
					_cur_pkt = nullptr;
					_cur_data = nullptr;
					_pkt_offset = 0;
					break;
				}

				if (_pkt->size > 0)
				{
					_pkt->pts = _parser->pts;
					_pkt->dts = _parser->dts;

					int ret = ::avcodec_send_packet(_context, _pkt);

					if (ret == AVERROR(EAGAIN))
					{
						// Need more data
						// *result = TranscodeResult::Again;
						break;
					}
					else if (ret == AVERROR_EOF)
					{
						logte("Error sending a packet for decoding : AVERROR_EOF");
						break;
					}
					else if (ret == AVERROR(EINVAL))
					{
						logte("Error sending a packet for decoding : AVERROR(EINVAL)");
						break;
					}
					else if (ret == AVERROR(ENOMEM))
					{
						logte("Error sending a packet for decoding : AVERROR(ENOMEM)");
						break;
					}
					else if (ret < 0)
					{
						logte("Error sending a packet for decoding : ERROR(Unknown %d)", ret);
						break;
					}
				}

				if (parsed_size > 0)
				{
					OV_ASSERT(_cur_data->GetLength() >= (size_t)parsed_size, "Current data size MUST greater than parsed_size, but data size: %ld, parsed_size: %ld", _cur_data->GetLength(), parsed_size);

					_pkt_offset += parsed_size;
				}

				break;
			}

			if (_cur_data->GetLength() <= _pkt_offset)
			{
				_cur_pkt = nullptr;
				_cur_data = nullptr;
				_pkt_offset = 0;
			}
		}

		/////////////////////////////////////////////////////////////////////
		// Receive a frame from decoder
		/////////////////////////////////////////////////////////////////////
		// Check the decoded frame is available
		int ret = ::avcodec_receive_frame(_context, _frame);
		if (ret == AVERROR(EAGAIN))
		{
			continue;
		}
		else if (ret == AVERROR_EOF)
		{
			logte("Error receiving a packet for decoding : AVERROR_EOF");
			continue;
		}
		else if (ret < 0)
		{
			logte("Error receiving a packet for decoding : %d", ret);
			continue;
		}
		else
		{
			bool need_to_change_notify = false;

			// Update codec informations if needed
			if (_change_format == false)
			{
				ret = ::avcodec_parameters_from_context(_codec_par, _context);

				if (ret == 0)
				{
					auto codec_info = ShowCodecParameters(_context, _codec_par);

					logti("[%s/%s(%u)] input stream information: %s",
						  _stream_info.GetApplicationInfo().GetName().CStr(), _stream_info.GetName().CStr(), _stream_info.GetId(), codec_info.CStr());

					_change_format = true;

					// If the format is changed, notify to another module
					need_to_change_notify = true;
				}
				else
				{
					logte("Could not obtain codec paramters from context %p", _context);
				}
			}

			_decoded_frame_num++;

			auto output_frame = std::make_shared<MediaFrame>();

			output_frame->SetMediaType(cmn::MediaType::Audio);
			output_frame->SetBytesPerSample(::av_get_bytes_per_sample(static_cast<AVSampleFormat>(_frame->format)));
			output_frame->SetNbSamples(_frame->nb_samples);
			output_frame->SetChannels(_frame->channels);
			output_frame->SetSampleRate(_frame->sample_rate);
			output_frame->SetFormat(_frame->format);

			// Calculate duration of frame in time_base
			float frame_duration_in_second = _frame->nb_samples * (1.0f / _frame->sample_rate);
			int frame_duration_in_timebase = static_cast<int>(frame_duration_in_second * _context->time_base.den);
			output_frame->SetDuration(frame_duration_in_timebase);

			// If the decoded audio frame does not have a PTS, Increase frame duration time in PTS of previous frame
			output_frame->SetPts(static_cast<int64_t>((_frame->pts == AV_NOPTS_VALUE) ? _last_pkt_pts + frame_duration_in_timebase : _frame->pts));
			_last_pkt_pts = output_frame->GetPts();

			// logte("frame.pts(%lld), oframe.pts(%lld),nb.samples(%d)", _frame->pts, output_frame->GetPts(), _frame->nb_samples);

			auto data_length = static_cast<uint32_t>(output_frame->GetBytesPerSample() * output_frame->GetNbSamples());

			// Copy frame data into out_buf
			if (TranscodeBase::IsPlanar(output_frame->GetFormat<AVSampleFormat>()))
			{
				// If the frame is planar, the data is stored separately in the "_frame->data" array.
				for (int channel = 0; channel < _frame->channels; channel++)
				{
					output_frame->Resize(data_length, channel);
					uint8_t *output = output_frame->GetWritableBuffer(channel);
					::memcpy(output, _frame->data[channel], data_length);
				}
			}
			else
			{
				// If the frame is non-planar, it means interleaved data. So, just copy from "_frame->data[0]" into the output_frame
				output_frame->AppendBuffer(_frame->data[0], data_length * _frame->channels, 0);
			}

			::av_frame_unref(_frame);

			// Return 1, if notification is required
			TranscodeResult result = need_to_change_notify ? TranscodeResult::FormatChanged : TranscodeResult::DataReady;

			_output_buffer.Enqueue(std::move(output_frame));
			OnCompleteHandler(result, _track_id);
		}
	}
}

std::shared_ptr<MediaFrame> OvenCodecImplAvcodecDecAAC::RecvBuffer(TranscodeResult *result)
{
	if (!_output_buffer.IsEmpty())
	{
		*result = TranscodeResult::DataReady;

		auto obj = _output_buffer.Dequeue();
		if (obj.has_value())
		{
			return std::move(obj.value());
		}
	}

	*result = TranscodeResult::NoData;
	return nullptr;
}
