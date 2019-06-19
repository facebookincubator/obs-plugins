/*
Copyright (C) 2019-present, Facebook, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <obs-module.h>
#include <obs-source.h>

#include <fcntl.h>  
#include <sys/types.h>  
#include <sys/stat.h>  
#include <io.h>  
#include <stdio.h>
#include <stdint.h>

#include <string>
#include <mutex>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

#pragma warning(push)
#pragma warning(disable:4244)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h>
}

#pragma warning(pop)

#include "oculus-mrc.h"
#include "frame.h"
#include "log.h"

#define OM_DEFAULT_WIDTH (1920*2)
#define OM_DEFAULT_HEIGHT 1080
#define OM_DEFAULT_AUDIO_SAMPLERATE 48000
#define OM_DEFAULT_IP_ADDRESS "192.168.0.1"
#define OM_DEFAULT_PORT 28734

std::string GetAvErrorString(int errNum)
{
	char buf[1024];
	std::string result = av_make_error_string(buf, 1024, errNum);
	return result;
}

class OculusMrcSource
{
public:
	// OBS source interfaces

	static const char* GetName(void*)
	{
		return obs_module_text("OculusMrcSource");
	}

	static void Update(void *data, obs_data_t *settings)
	{
		OculusMrcSource *context = (OculusMrcSource*)data;
		context->Update(settings);
	}

	static void *Create(obs_data_t *settings, obs_source_t *source)
	{
		OculusMrcSource *context = new OculusMrcSource(source);
		Update(context, settings);
		return context;
	}

	static void Destroy(void *data)
	{
		delete (OculusMrcSource*)data;
	}

	static obs_properties_t *GetProperties(void *source)
	{
		OculusMrcSource* context = (OculusMrcSource*)source;

		obs_properties_t *props = obs_properties_create();

		obs_properties_add_text(props, "ipaddr",
			obs_module_text("IpAddress"), OBS_TEXT_DEFAULT);

		obs_properties_add_int(props, "port", obs_module_text("Port"), 1025, 65535, 1);

		obs_property_t* connectButton = obs_properties_add_button(props, "connect",
			obs_module_text("Connect"), [](obs_properties_t *props,
				obs_property_t *property, void *data) {
			return ((OculusMrcSource *)data)->ConnectClicked(props, property);
		});
		obs_property_set_enabled(connectButton, context->m_connectSocket == INVALID_SOCKET);

		obs_property_t* disconnectButton = obs_properties_add_button(props, "disconnect",
			obs_module_text("Disconnect"), [](obs_properties_t *props,
				obs_property_t *property, void *data) {
			return ((OculusMrcSource *)data)->DisconnectClicked(props, property);
		});
		obs_property_set_enabled(disconnectButton, context->m_connectSocket != INVALID_SOCKET);

		return props;
	}

	static void VideoTick(void *data, float seconds)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		context->VideoTick(seconds);
	}

	static void VideoRender(void *data, gs_effect_t *effect)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		context->VideoRender(effect);
	}

	static uint32_t GetWidth(void *data)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		return context->GetWidth();
	}

	static uint32_t GetHeight(void *data)
	{
		OculusMrcSource *context = (OculusMrcSource *)data;
		return context->GetHeight();
	}

	static void GetDefaults(obs_data_t *settings)
	{
		obs_data_set_default_int(settings, "width", OM_DEFAULT_WIDTH);
		obs_data_set_default_int(settings, "height", OM_DEFAULT_HEIGHT);
		obs_data_set_default_string(settings, "ipaddr", OM_DEFAULT_IP_ADDRESS);
		obs_data_set_default_int(settings, "port", OM_DEFAULT_PORT);
	}

	void VideoTick(float /*seconds*/)
	{
		std::lock_guard<std::mutex> lock(m_updateMutex);
		VideoTickImpl();
	}

	void VideoRender(gs_effect_t* /*effect*/)
	{
		//std::lock_guard<std::mutex> lock(m_updateMutex);
		VideoRenderImpl();
	}

	void RefreshButtons(obs_properties_t* props)
	{
		obs_property_set_enabled(obs_properties_get(props, "connect"), m_connectSocket == INVALID_SOCKET);
		obs_property_set_enabled(obs_properties_get(props, "disconnect"), m_connectSocket != INVALID_SOCKET);
	}

	bool ConnectClicked(obs_properties_t* props, obs_property_t* /*property*/) {
		OM_BLOG(LOG_INFO, "ConnectClicked");

		std::lock_guard<std::mutex> lock(m_updateMutex);
		Connect();
		RefreshButtons(props);

		return true;
	}

	bool DisconnectClicked(obs_properties_t* props, obs_property_t* /*property*/) {
		OM_BLOG(LOG_INFO, "DisconnectClicked");

		std::lock_guard<std::mutex> lock(m_updateMutex);
		Disconnect();
		RefreshButtons(props);

		return true;
	}

private:
	OculusMrcSource(obs_source_t* source) :
		m_src(source)
	{
		m_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!m_codec)
		{
			OM_BLOG(LOG_ERROR, "Unable to find decoder");
		}
		else
		{
			OM_BLOG(LOG_INFO, "Codec found. Capabilities 0x%x", m_codec->capabilities);
		}

		obs_enter_graphics();
		char *filename = obs_module_file("oculusmrc.effect");
		m_mrc_effect = gs_effect_create_from_file(filename,
			NULL);
		bfree(filename);
		assert(m_mrc_effect);
		obs_leave_graphics();
	}

	~OculusMrcSource()
	{
		StopDecoder();
		if (m_mrc_effect)
		{
			gs_effect_destroy(m_mrc_effect);
			m_mrc_effect = nullptr;
		}
		if (m_temp_texture)
		{
			gs_texture_destroy(m_temp_texture);
			m_temp_texture = nullptr;
		}
		if (m_swsContext)
		{
			sws_freeContext(m_swsContext);
			m_swsContext = nullptr;
		}
	}

	void StartDecoder()
	{
		if (m_codecContext != nullptr)
		{
			OM_BLOG(LOG_ERROR, "Decoder already started");
			return;
		}

		if (!m_codec)
		{
			OM_BLOG(LOG_ERROR, "m_codec not initalized");
			return;
		}

		m_codecContext = avcodec_alloc_context3(m_codec);
		if (!m_codecContext)
		{
			OM_BLOG(LOG_ERROR, "Unable to create codec context");
			return;
		}

		AVDictionary* dict = nullptr;
		int ret = avcodec_open2(m_codecContext, m_codec, &dict);
		av_dict_free(&dict);
		if (ret < 0)
		{
			OM_BLOG(LOG_ERROR, "Unable to open codec context");
			avcodec_free_context(&m_codecContext);
			return;
		}

		OM_BLOG(LOG_INFO, "m_codecContext constructed and opened");
	}

	void StopDecoder()
	{
		if (m_codecContext)
		{
			avcodec_close(m_codecContext);
			avcodec_free_context(&m_codecContext);
			OM_BLOG(LOG_INFO, "m_codecContext freed");
		}

		if (m_temp_texture)
		{
			gs_texture_destroy(m_temp_texture);
			m_temp_texture = nullptr;
		}
	}

	// settings
	uint32_t m_width = OM_DEFAULT_WIDTH;
	uint32_t m_height = OM_DEFAULT_HEIGHT;
	uint32_t m_audioSampleRate = OM_DEFAULT_AUDIO_SAMPLERATE;
	std::string m_ipaddr = OM_DEFAULT_IP_ADDRESS;
	uint32_t m_port = OM_DEFAULT_PORT;

	std::mutex m_updateMutex;

	obs_source_t *m_src = nullptr;
	gs_texture_t * m_temp_texture = nullptr;
	gs_effect_t* m_mrc_effect = nullptr;

	AVCodec* m_codec = nullptr;
	AVCodecContext* m_codecContext = nullptr;

	SOCKET m_connectSocket = INVALID_SOCKET;
	FrameCollection m_frameCollection;

	SwsContext* m_swsContext = nullptr;
	int m_swsContext_SrcWidth = 0;
	int m_swsContext_SrcHeight = 0;
	AVPixelFormat m_swsContext_SrcPixelFormat = AV_PIX_FMT_NONE;
	int m_swsContext_DestWidth = 0;
	int m_swsContext_DestHeight = 0;

	std::vector<std::pair<int, std::shared_ptr<Frame>>> m_cachedAudioFrames;
	int m_audioFrameIndex = 0;
	int m_videoFrameIndex = 0;

	void Update(obs_data_t* settings)
	{
		m_width = (uint32_t)obs_data_get_int(settings, "width");
		m_height = (uint32_t)obs_data_get_int(settings, "height");
		m_ipaddr = obs_data_get_string(settings, "ipaddr");
		m_port = (uint32_t)obs_data_get_int(settings, "port");
	}

	uint32_t GetWidth()
	{
		return m_width;
	}

	uint32_t GetHeight()
	{
		return m_height;
	}

	void ReceiveData()
	{
		if (m_connectSocket != INVALID_SOCKET)
		{
			for (;;)
			{
				fd_set socketSet = { 0 };
				FD_ZERO(&socketSet);
				FD_SET(m_connectSocket, &socketSet);

				timeval t = { 0, 0 };
				int num = select(0, &socketSet, nullptr, nullptr, &t);
				if (num >= 1)
				{
					const int bufferSize = 65536;
					uint8_t buf[bufferSize];
					int iResult = recv(m_connectSocket, (char*)buf, bufferSize, 0);
					if (iResult < 0)
					{
						OM_BLOG(LOG_ERROR, "recv error %d, closing socket", iResult);
						Disconnect();
					}
					else if (iResult == 0)
					{
						OM_BLOG(LOG_INFO, "recv 0 bytes, closing socket");
						Disconnect();
					}
					else
					{
						//OM_BLOG(LOG_INFO, "recv: %d bytes received", iResult);
						m_frameCollection.AddData(buf, iResult);
					}
				}
				else
				{
					break;
				}
			}
		}
	}

	void VideoTickImpl()
	{
		if (m_connectSocket != INVALID_SOCKET)
		{
			ReceiveData();

			if (m_connectSocket == INVALID_SOCKET)	// socket disconnected
				return;

			//std::chrono::time_point<std::chrono::system_clock> startTime = std::chrono::system_clock::now();
			while (m_frameCollection.HasCompletedFrame())
			{
				//std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - startTime;
				//if (timePassed.count() > 0.05)
				//	break;

				auto frame = m_frameCollection.PopFrame();

				//auto current_time = std::chrono::system_clock::now();
				//auto seconds_since_epoch = std::chrono::duration<double>(current_time.time_since_epoch()).count();
				//double latency = seconds_since_epoch - frame->m_secondsSinceEpoch;

				if (frame->m_type == Frame::PayloadType::VIDEO_DIMENSION)
				{
					struct FrameDimension
					{
						int w;
						int h;
					};
					const FrameDimension* dim = (const FrameDimension*)frame->m_payload.data();
					m_width = dim->w;
					m_height = dim->h;

					OM_BLOG(LOG_INFO, "[VIDEO_DIMENSION] width %d height %d", m_width, m_height);
				}
				else if (frame->m_type == Frame::PayloadType::VIDEO_DATA)
				{
					AVPacket* packet = av_packet_alloc();
					AVFrame* picture = av_frame_alloc();

					av_new_packet(packet, (int)frame->m_payload.size());
					assert(packet->data);
					memcpy(packet->data, frame->m_payload.data(), frame->m_payload.size());

					int ret = avcodec_send_packet(m_codecContext, packet);
					if (ret < 0)
					{
						OM_BLOG(LOG_ERROR, "avcodec_send_packet error %s", GetAvErrorString(ret).c_str());
					}
					else
					{
						ret = avcodec_receive_frame(m_codecContext, picture);
						if (ret < 0)
						{
							OM_BLOG(LOG_ERROR, "avcodec_receive_frame error %s", GetAvErrorString(ret).c_str());
						}
						else
						{
#if _DEBUG
							std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
							OM_BLOG(LOG_DEBUG, "[%f][VIDEO_DATA] size %d width %d height %d format %d", timePassed.count(), packet->size, picture->width, picture->height, picture->format);
#endif

							while (m_cachedAudioFrames.size() > 0 && m_cachedAudioFrames[0].first <= m_videoFrameIndex)
							{
								std::shared_ptr<Frame> audioFrame = m_cachedAudioFrames[0].second;

								struct AudioDataHeader {
									uint64_t timestamp;
									int channels;
									int dataLength;
								};
								AudioDataHeader* audioDataHeader = (AudioDataHeader*)(audioFrame->m_payload.data());

								if (audioDataHeader->channels == 1 || audioDataHeader->channels == 2)
								{
									obs_source_audio audio = { 0 };
									audio.data[0] = (uint8_t*)audioFrame->m_payload.data() + sizeof(AudioDataHeader);
									audio.frames = audioDataHeader->dataLength / sizeof(float) / audioDataHeader->channels;
									audio.speakers = audioDataHeader->channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
									audio.format = AUDIO_FORMAT_FLOAT;
									audio.samples_per_sec = m_audioSampleRate;
									audio.timestamp = audioDataHeader->timestamp;
									obs_source_output_audio(m_src, &audio);
								}
								else
								{
									OM_BLOG(LOG_ERROR, "[AUDIO_DATA] unimplemented audio channels %d", audioDataHeader->channels);
								}

								m_cachedAudioFrames.erase(m_cachedAudioFrames.begin());
							}

							++m_videoFrameIndex;

							if (m_swsContext != nullptr)
							{
								if (m_swsContext_SrcWidth != m_codecContext->width ||
									m_swsContext_SrcHeight != m_codecContext->height ||
									m_swsContext_SrcPixelFormat != m_codecContext->pix_fmt ||
									m_swsContext_DestWidth != m_codecContext->width ||
									m_swsContext_DestHeight != m_codecContext->height)
								{
									OM_BLOG(LOG_DEBUG, "Need recreate m_swsContext");
									sws_freeContext(m_swsContext);
									m_swsContext = nullptr;
								}
							}

							if (m_swsContext == nullptr)
							{
								m_swsContext = sws_getContext(
									m_codecContext->width,
									m_codecContext->height,
									m_codecContext->pix_fmt,
									m_codecContext->width,
									m_codecContext->height,
									AV_PIX_FMT_RGBA,
									SWS_POINT,
									nullptr, nullptr, nullptr
								);
								m_swsContext_SrcWidth = m_codecContext->width;
								m_swsContext_SrcHeight = m_codecContext->height;
								m_swsContext_SrcPixelFormat = m_codecContext->pix_fmt;
								m_swsContext_DestWidth = m_codecContext->width;
								m_swsContext_DestHeight = m_codecContext->height;
								OM_BLOG(LOG_DEBUG, "sws_getContext(%d, %d, %d)", m_codecContext->width, m_codecContext->height, m_codecContext->pix_fmt);
							}

							assert(m_swsContext);
							uint8_t* data[1] = { new uint8_t[m_codecContext->width * m_codecContext->height * 4] };
							int stride[1] = { (int)m_codecContext->width * 4 };
							sws_scale(m_swsContext, picture->data,
								picture->linesize,
								0,
								picture->height,
								data,
								stride);

							obs_enter_graphics();
							if (m_temp_texture)
							{
								gs_texture_destroy(m_temp_texture);
								m_temp_texture = nullptr;
							}
							m_temp_texture = gs_texture_create(m_codecContext->width,
								m_codecContext->height,
								GS_RGBA,
								1,
								const_cast<const uint8_t**>(&data[0]),
								0);
							obs_leave_graphics();

							delete data[0];
						}
					}

					av_frame_free(&picture);
					av_packet_free(&packet);
				}
				else if (frame->m_type == Frame::PayloadType::AUDIO_SAMPLERATE)
				{
					m_audioSampleRate = *(uint32_t*)(frame->m_payload.data());
					OM_BLOG(LOG_DEBUG, "[AUDIO_SAMPLERATE] %d", m_audioSampleRate);
				}
				else if (frame->m_type == Frame::PayloadType::AUDIO_DATA)
				{
					m_cachedAudioFrames.push_back(std::make_pair(m_audioFrameIndex, frame));
					++m_audioFrameIndex;
#if _DEBUG
					std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
					OM_BLOG(LOG_DEBUG, "[%f][AUDIO_DATA] timestamp %llu", timePassed.count());
#endif
				}
				else
				{
					OM_BLOG(LOG_ERROR, "Unknown payload type: %u", frame->m_type);
				}
			}
		}
	}

	void VideoRenderImpl()
	{
#if _DEBUG
		if (m_connectSocket != INVALID_SOCKET && m_frameCollection.HasFirstFrame())
		{
			std::chrono::duration<double> timePassed = std::chrono::system_clock::now() - m_frameCollection.GetFirstFrameTime();
			OM_BLOG(LOG_DEBUG, "[%f] VideoRenderImpl", timePassed.count());
		}
#endif

		if (m_temp_texture)
		{
			gs_technique_t *tech = gs_effect_get_technique(m_mrc_effect, "Frame");

			gs_technique_begin(tech);
			gs_technique_begin_pass(tech, 0);

			obs_source_draw(m_temp_texture, 0, 0, m_width, m_height, true);

			gs_technique_end_pass(tech);
			gs_technique_end(tech);
		}
		else
		{
			gs_technique_t *tech = gs_effect_get_technique(m_mrc_effect, "Empty");

			gs_technique_begin(tech);
			gs_technique_begin_pass(tech, 0);

			gs_draw_sprite(0, 0, m_width, m_height);

			gs_technique_end_pass(tech);
			gs_technique_end(tech);
		}
	}

	void Connect()
	{
		if (m_connectSocket != INVALID_SOCKET)
		{
			OM_BLOG(LOG_ERROR, "Already connected");
			return;
		}

		struct addrinfo *result = NULL;
		struct addrinfo *ptr = NULL;
		struct addrinfo hints = { 0 };

		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		int iResult;
		iResult = getaddrinfo(m_ipaddr.c_str(), std::to_string(m_port).c_str(), &hints, &result);
		if (iResult != 0)
		{
			OM_BLOG(LOG_ERROR, "getaddrinfo failed: %d", iResult);
		}

		ptr = result;
		m_connectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (m_connectSocket == INVALID_SOCKET)
		{
			OM_BLOG(LOG_ERROR, "Error at socket(): %d", WSAGetLastError());
			freeaddrinfo(result);
		}

		iResult = connect(m_connectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
		if (iResult == SOCKET_ERROR)
		{
			OM_BLOG(LOG_ERROR, "Unable to connect");
			closesocket(m_connectSocket);
			m_connectSocket = INVALID_SOCKET;
		}

		OM_BLOG(LOG_INFO, "Socket connected to %s:%d", m_ipaddr.c_str(), m_port);

		freeaddrinfo(result);

		m_frameCollection.Reset();

		m_audioFrameIndex = 0;
		m_videoFrameIndex = 0;
		m_cachedAudioFrames.clear();

		StartDecoder();
	}

	void Disconnect()
	{
		if (m_connectSocket == INVALID_SOCKET)
		{
			OM_BLOG(LOG_ERROR, "Not connected");
			return;
		}

		StopDecoder();

		int ret = closesocket(m_connectSocket);
		if (ret == INVALID_SOCKET)
		{
			OM_BLOG(LOG_ERROR, "closesocket error %d", WSAGetLastError());
		}
		m_connectSocket = INVALID_SOCKET;
		OM_BLOG(LOG_INFO, "Socket disconnected");
	}

};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("oculus-mrc", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Oculus MRC source";
}

bool obs_module_load(void)
{
	// Initialize Winsock
	WSADATA wsaData = { 0 };
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		OM_LOG(LOG_ERROR, "WSAStartup failed: %d\n", iResult);
		return false;
	}

	struct obs_source_info oculus_mrc_source_info = { 0 };
	oculus_mrc_source_info.id = "oculus_mrc_source";
	oculus_mrc_source_info.type = OBS_SOURCE_TYPE_INPUT;
	oculus_mrc_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO/* | OBS_SOURCE_CUSTOM_DRAW*/;
	oculus_mrc_source_info.create = &OculusMrcSource::Create;
	oculus_mrc_source_info.destroy = &OculusMrcSource::Destroy;
	oculus_mrc_source_info.update = &OculusMrcSource::Update;
	oculus_mrc_source_info.get_name = &OculusMrcSource::GetName;
	oculus_mrc_source_info.get_defaults = &OculusMrcSource::GetDefaults;
	oculus_mrc_source_info.get_width = &OculusMrcSource::GetWidth;
	oculus_mrc_source_info.get_height = &OculusMrcSource::GetHeight;
	oculus_mrc_source_info.video_tick = &OculusMrcSource::VideoTick;
	oculus_mrc_source_info.video_render = &OculusMrcSource::VideoRender;
	oculus_mrc_source_info.get_properties = &OculusMrcSource::GetProperties;

	obs_register_source(&oculus_mrc_source_info);
	return true;
}

void obs_module_unload(void)
{
	WSACleanup();
}
