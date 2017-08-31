#include "PrecompiledHeader.hpp"
#include "Interface\Application\Application.hpp"

extern "C"
{
	#include "libavutil\avutil.h"
	#include "libavutil\imgutils.h"
	#include "libavcodec\avcodec.h"
	#include "libavformat\avformat.h"
}

#include <ppltasks.h>

/*
	For WAVE related things.
*/
#include <mmsystem.h>

#include <d3d9.h>
#include <d3d11.h>

#include "readerwriterqueue.h"

#include "Shaders\Blobs\BGR0.hpp"
#include "Shaders\Blobs\ClearUAV.hpp"
#include "Shaders\Blobs\PassUAV.hpp"
#include "Shaders\Blobs\Sampling.hpp"
#include "Shaders\Blobs\YUV420.hpp"
#include "Shaders\Blobs\YUV444.hpp"

#include "Interface\Application\Modules\Shared\EngineClient.hpp"
#include "Interface\Application\Modules\Shared\MaterialSystem.hpp"
#include "Interface\Application\Modules\Shared\SourceGlobals.hpp"

namespace
{
	namespace Variables
	{
		SDR::Console::Variable OutputDirectory;
		SDR::Console::Variable FlashWindow;
		SDR::Console::Variable ExitOnFinish;
		SDR::Console::Variable SuppressLog;

		namespace Video
		{
			SDR::Console::Variable Framerate;
			SDR::Console::Variable ColorSpace;

			namespace Sample
			{
				SDR::Console::Variable Multiply;
				SDR::Console::Variable Exposure;
			}

			SDR::Console::Variable Encoder;
			SDR::Console::Variable PixelFormat;

			namespace D3D11
			{
				SDR::Console::Variable Staging;
			}

			namespace X264
			{
				SDR::Console::Variable CRF;
				SDR::Console::Variable Preset;
				SDR::Console::Variable Intra;
			}
		}

		/*
			Creation has to be delayed as the necessary console stuff isn't available earlier.
		*/
		SDR::PluginStartupFunctionAdder A1("MovieRecord console variables", []()
		{
			OutputDirectory = SDR::Console::MakeString("sdr_outputdir", "");
			FlashWindow = SDR::Console::MakeBool("sdr_endmovieflash", "0");
			ExitOnFinish = SDR::Console::MakeBool("sdr_endmoviequit", "0");
			SuppressLog = SDR::Console::MakeBool("sdr_movie_suppresslog", "1");

			Video::Framerate = SDR::Console::MakeNumber("sdr_render_framerate", "60", 30, 1000);
			Video::ColorSpace = SDR::Console::MakeString("sdr_movie_encoder_colorspace", "709");

			Video::Sample::Multiply = SDR::Console::MakeNumber("sdr_sample_mult", "32", 0);
			Video::Sample::Exposure = SDR::Console::MakeNumber("sdr_sample_exposure", "0.5", 0, 1);

			Video::Encoder = SDR::Console::MakeString("sdr_movie_encoder", "libx264");
			Video::PixelFormat = SDR::Console::MakeString("sdr_movie_encoder_pxformat", "");

			Video::D3D11::Staging = SDR::Console::MakeBool("sdr_d3d11_staging", "1");

			Video::X264::CRF = SDR::Console::MakeNumberWithString("sdr_x264_crf", "0", 0, 51);
			Video::X264::Preset = SDR::Console::MakeString("sdr_x264_preset", "ultrafast");
			Video::X264::Intra = SDR::Console::MakeBool("sdr_x264_intra", "1");
		});
	}
}

namespace
{
	namespace LAV
	{
		struct ScopedFormatContext
		{
			~ScopedFormatContext()
			{
				if (Context)
				{
					if (!(Context->oformat->flags & AVFMT_NOFILE))
					{
						avio_close(Context->pb);
					}

					avformat_free_context(Context);
				}
			}

			void Assign(const char* filename)
			{
				SDR::Error::LAV::ThrowIfFailed
				(
					avformat_alloc_output_context2(&Context, nullptr, nullptr, filename),
					"Could not allocate output context for \"%s\"", filename
				);
			}

			AVFormatContext* Get() const
			{
				return Context;
			}

			auto operator->() const
			{
				return Get();
			}

			AVFormatContext* Context = nullptr;
		};

		struct ScopedAVFrame
		{
			~ScopedAVFrame()
			{
				if (Frame)
				{
					av_frame_free(&Frame);
				}
			}

			void Assign(int width, int height, AVPixelFormat format, AVColorSpace colorspace, AVColorRange colorrange)
			{
				Frame = av_frame_alloc();

				SDR::Error::ThrowIfNull(Frame, "Could not allocate video frame");

				Frame->format = format;
				Frame->width = width;
				Frame->height = height;
				Frame->colorspace = colorspace;
				Frame->color_range = colorrange;

				av_frame_get_buffer(Frame, 32);

				Frame->pts = 0;
			}

			AVFrame* Get() const
			{
				return Frame;
			}

			auto operator->() const
			{
				return Get();
			}

			AVFrame* Frame = nullptr;
		};

		struct ScopedAVDictionary
		{
			~ScopedAVDictionary()
			{
				av_dict_free(&Options);
			}

			AVDictionary** Get()
			{
				return &Options;
			}

			void Set(const char* key, const char* value, int flags = 0)
			{
				SDR::Error::LAV::ThrowIfFailed
				(
					av_dict_set(Get(), key, value, flags),
					"Could not set dictionary value { \"%s\" = \"%s\" }", key, value
				);
			}

			AVDictionary* Options = nullptr;
		};

		void LogFunction(void* avcl, int level, const char* fmt, va_list vl)
		{
			if (!Variables::SuppressLog.GetBool())
			{
				/*
					989 max limit according to
					https://developer.valvesoftware.com/wiki/Developer_Console_Control#Printing_to_the_console

					960 to keep in a 32 byte alignment.
				*/
				char buf[960];
				vsprintf_s(buf, fmt, vl);

				/*
					Not formatting the buffer to a string will create a runtime error on any float conversion.
				*/
				SDR::Log::Message("%s", buf);
			}
		}
	}
}

namespace
{
	namespace Profile
	{
		const char* Names[] =
		{
			"PushYUV",
			"PushRGB",
			"Encode",
		};

		namespace Types
		{
			enum Type
			{
				PushYUV,
				PushRGB,
				Encode,

				Count
			};
		}

		auto GetTimeNow()
		{
			return std::chrono::high_resolution_clock::now();
		}

		using TimePointType = decltype(GetTimeNow());

		std::chrono::nanoseconds GetTimeDifference(TimePointType start)
		{
			using namespace std::chrono;

			auto now = GetTimeNow();
			auto difference = now - start;

			auto time = duration_cast<nanoseconds>(difference);

			return time;
		}

		struct Entry
		{
			uint32_t Calls = 0;
			std::chrono::nanoseconds TotalTime = 0ns;
		};

		std::array<Entry, Types::Count> Entries;

		struct ScopedEntry
		{
			ScopedEntry(Types::Type entry) :
				Target(Entries[entry]),
				Start(GetTimeNow())
			{
				++Target.Calls;
			}

			~ScopedEntry()
			{
				Target.TotalTime += GetTimeDifference(Start);
			}

			TimePointType Start;
			Entry& Target;
		};

		void Reset()
		{
			Entries.fill({});
		}

		void ShowResults()
		{
			int index = 0;

			for (const auto& entry : Entries)
			{
				if (entry.Calls > 0)
				{
					auto name = Names[index];
					auto avg = entry.TotalTime / entry.Calls;
					auto ms = avg / 1.0ms;

					SDR::Log::Message("SDR: %s (%u): avg %0.4f ms\n", name, entry.Calls, ms);
				}

				++index;
			}
		}
	}

	struct SDRVideoWriter
	{
		/*
			At most, YUV formats will use all planes. RGB only uses 1.
		*/
		using PlaneType = std::array<std::vector<uint8_t>, 3>;

		void OpenFileForWrite(const char* path)
		{
			FormatContext.Assign(path);

			if ((FormatContext->oformat->flags & AVFMT_NOFILE) == 0)
			{
				SDR::Error::LAV::ThrowIfFailed
				(
					avio_open(&FormatContext->pb, path, AVIO_FLAG_WRITE),
					"Could not open output file for \"%s\"", path
				);
			}
		}

		void SetEncoder(AVCodec* encoder)
		{
			Encoder = encoder;

			Stream = avformat_new_stream(FormatContext.Get(), Encoder);
			SDR::Error::ThrowIfNull(Stream, "Could not create video stream");

			/*
				Against what the new ffmpeg API incorrectly suggests, but is the right way.
			*/
			CodecContext = Stream->codec;
		}

		void OpenEncoder(int framerate, AVDictionary** options)
		{
			CodecContext->width = Frame->width;
			CodecContext->height = Frame->height;
			CodecContext->pix_fmt = (AVPixelFormat)Frame->format;
			CodecContext->colorspace = Frame->colorspace;
			CodecContext->color_range = Frame->color_range;

			if (FormatContext->oformat->flags & AVFMT_GLOBALHEADER)
			{
				CodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}

			AVRational timebase;
			timebase.num = 1;
			timebase.den = framerate;

			auto inversetime = av_inv_q(timebase);

			CodecContext->time_base = timebase;
			CodecContext->framerate = inversetime;

			SDR::Error::LAV::ThrowIfFailed
			(
				avcodec_open2(CodecContext, Encoder, options),
				"Could not open video encoder"
			);

			SDR::Error::LAV::ThrowIfFailed
			(
				avcodec_parameters_from_context(Stream->codecpar, CodecContext),
				"Could not transfer encoder parameters to stream"
			);

			Stream->time_base = timebase;
			Stream->avg_frame_rate = inversetime;
		}

		void WriteHeader()
		{
			SDR::Error::LAV::ThrowIfFailed
			(
				avformat_write_header(FormatContext.Get(), nullptr),
				"Could not write container header"
			);
		}

		void WriteTrailer()
		{
			av_write_trailer(FormatContext.Get());
		}

		void SetFrameInput(PlaneType& planes)
		{
			int index = 0;

			for (auto& plane : planes)
			{
				if (plane.empty())
				{
					break;
				}

				Frame->data[index] = plane.data();
				++index;
			}
		}

		void SendRawFrame()
		{
			{
				Profile::ScopedEntry e1(Profile::Types::Encode);

				Frame->pts = PresentationIndex;
				PresentationIndex++;

				avcodec_send_frame(CodecContext, Frame.Get());
			}

			ReceivePacketFrame();
		}

		void SendFlushFrame()
		{
			avcodec_send_frame(CodecContext, nullptr);
			ReceivePacketFrame();
		}

		void Finish()
		{
			SendFlushFrame();
			WriteTrailer();
		}

		void ReceivePacketFrame()
		{
			int status = 0;

			AVPacket packet = {};
			av_init_packet(&packet);

			while (status == 0)
			{
				status = avcodec_receive_packet(CodecContext, &packet);

				if (status < 0)
				{
					return;
				}

				WriteEncodedPacket(packet);
			}
		}

		void WriteEncodedPacket(AVPacket& packet)
		{
			av_packet_rescale_ts(&packet, CodecContext->time_base, Stream->time_base);

			packet.stream_index = Stream->index;

			av_interleaved_write_frame(FormatContext.Get(), &packet);
		}

		LAV::ScopedFormatContext FormatContext;

		/*
			This gets freed when FormatContext gets destroyed.
		*/
		AVCodecContext* CodecContext;
		AVCodec* Encoder = nullptr;
		AVStream* Stream = nullptr;
		LAV::ScopedAVFrame Frame;

		/*
			Incremented for every sent frame.
		*/
		int64_t PresentationIndex = 0;
	};

	struct MovieData
	{
		bool IsStarted = false;

		int OldMatQueueModeValue;

		/*
			Whether to use an extra intermediate buffer for GPU -> CPU transfer.
		*/
		static bool UseStaging()
		{
			return Variables::Video::D3D11::Staging.GetBool();
		}

		static bool UseSampling()
		{
			auto exposure = Variables::Video::Sample::Exposure.GetFloat();
			auto mult = Variables::Video::Sample::Multiply.GetInt();

			return mult > 1 && exposure > 0;
		}

		static void OpenShader(ID3D11Device* device, const char* name, const BYTE* data, size_t size, ID3D11ComputeShader** shader)
		{
			SDR::Error::MS::ThrowIfFailed
			(
				device->CreateComputeShader(data, size, nullptr, shader),
				"Could not create compute shader \"%s\"", name
			);
		}

		static bool WouldNewFrameOverflow()
		{
			PROCESS_MEMORY_COUNTERS desc = {};

			auto res = K32GetProcessMemoryInfo(GetCurrentProcess(), &desc, sizeof(desc));

			if (res == 0)
			{
				SDR::Log::Warning("SDR: Could not retrieve process memory info\n"s);
				return true;
			}

			return desc.WorkingSetSize > INT32_MAX;
		}

		/*
			This structure is sent to the encoder thread from the capture thread.
		*/
		struct VideoFutureData
		{
			SDRVideoWriter* Writer;
			SDRVideoWriter::PlaneType Planes;
		};

		/*
			A lock-free producer/consumer queue.
		*/
		using VideoQueueType = moodycamel::ReaderWriterQueue<VideoFutureData>;

		struct VideoStreamSharedData
		{
			struct DirectX11Data
			{
				void Create(int width, int height)
				{
					uint32_t flags = 0;
					#ifdef _DEBUG
					flags |= D3D11_CREATE_DEVICE_DEBUG;
					#endif

					SDR::Error::MS::ThrowIfFailed
					(
						D3D11CreateDevice
						(
							nullptr,
							D3D_DRIVER_TYPE_HARDWARE,
							0,
							flags,
							nullptr,
							0,
							D3D11_SDK_VERSION,
							Device.GetAddressOf(),
							nullptr,
							Context.GetAddressOf()
						),
						"Could not create D3D11 device"
					);

					/*
						Divisors must match number of threads in SharedAll.hlsl.
					*/
					GroupsX = std::ceil(width / 8.0);
					GroupsY = std::ceil(height / 8.0);

					if (UseSampling())
					{
						D3D11_BUFFER_DESC cbufdesc = {};
						cbufdesc.ByteWidth = sizeof(SamplingConstantData);
						cbufdesc.Usage = D3D11_USAGE_DYNAMIC;
						cbufdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
						cbufdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

						SDR::Error::MS::ThrowIfFailed
						(
							Device->CreateBuffer(&cbufdesc, nullptr, SamplingConstantBuffer.GetAddressOf()),
							"Could not create sampling constant buffer"
						);
					}

					{
						/*
							Matches SharedInputData in SharedAll.hlsl.
						*/
						__declspec(align(16)) struct
						{
							int Dimensions[2];
						} cbufdata;

						cbufdata.Dimensions[0] = width;
						cbufdata.Dimensions[1] = height;

						D3D11_BUFFER_DESC cbufdesc = {};
						cbufdesc.ByteWidth = sizeof(cbufdata);
						cbufdesc.Usage = D3D11_USAGE_IMMUTABLE;
						cbufdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

						D3D11_SUBRESOURCE_DATA cbufsubdesc = {};
						cbufsubdesc.pSysMem = &cbufdata;

						SDR::Error::MS::ThrowIfFailed
						(
							Device->CreateBuffer(&cbufdesc, &cbufsubdesc, SharedConstantBuffer.GetAddressOf()),
							"Could not create constant buffer for shared shader data"
						);
					}

					if (UseSampling())
					{
						OpenShader(Device.Get(), "Sampling", CSBlob_Sampling, sizeof(CSBlob_Sampling), SamplingShader.GetAddressOf());
						OpenShader(Device.Get(), "ClearUAV", CSBlob_ClearUAV, sizeof(CSBlob_ClearUAV), ClearShader.GetAddressOf());
					}

					else
					{
						OpenShader(Device.Get(), "PassUAV", CSBlob_PassUAV, sizeof(CSBlob_PassUAV), PassShader.GetAddressOf());
					}
				}

				Microsoft::WRL::ComPtr<ID3D11Device> Device;
				Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;

				int GroupsX;
				int GroupsY;

				/*
					Contains the current video frame dimensions. Will always be bound at slot 0.
				*/
				Microsoft::WRL::ComPtr<ID3D11Buffer> SharedConstantBuffer;

				__declspec(align(16)) struct
				{
					float Weight;
				} SamplingConstantData;

				Microsoft::WRL::ComPtr<ID3D11Buffer> SamplingConstantBuffer;
				Microsoft::WRL::ComPtr<ID3D11ComputeShader> SamplingShader;

				/*
					Shader for setting every UAV structure color to 0.
				*/
				Microsoft::WRL::ComPtr<ID3D11ComputeShader> ClearShader;

				/*
					When no sampling is enabled, this shader just takes the game backbuffer texture and puts it into WorkBuffer.
				*/
				Microsoft::WRL::ComPtr<ID3D11ComputeShader> PassShader;
			} DirectX11;
		} VideoStreamShared;

		struct VideoStreamBase
		{
			struct DirectX9Data
			{
				struct SharedSurfaceData
				{
					void Create(IDirect3DDevice9Ex* device, int width, int height)
					{
						SDR::Error::MS::ThrowIfFailed
						(
							/*
								Once shared with D3D11, it is interpreted as
								DXGI_FORMAT_B8G8R8A8_UNORM.

								Previously "CreateOffscreenPlainSurface" but that function
								produced black output for some users.

								According to MSDN (https://msdn.microsoft.com/en-us/library/windows/desktop/ff476531(v=vs.85).aspx)
								the flags for the texture needs to be RENDER_TARGET which I guess the previous function didn't set.
								MSDN also mentions an non-existent SHADER_RESOURCE flag which seems safe to omit.

								This change was first made in:
								https://github.com/crashfort/SourceDemoRender/commit/eaabd701ce413cc372aeabe57755ce37e4bf741c
							*/
							device->CreateTexture
							(
								width,
								height,
								1,
								D3DUSAGE_RENDERTARGET,
								D3DFMT_A8R8G8B8,
								D3DPOOL_DEFAULT,
								Texture.GetAddressOf(),
								&SharedHandle
							),
							"Could not create D3D9 shared texture"
						);

						SDR::Error::MS::ThrowIfFailed
						(
							Texture->GetSurfaceLevel(0, Surface.GetAddressOf()),
							"Could not get D3D9 surface from texture"
						);
					}

					HANDLE SharedHandle = nullptr;
					Microsoft::WRL::ComPtr<IDirect3DTexture9> Texture;
					Microsoft::WRL::ComPtr<IDirect3DSurface9> Surface;
				};

				void Create(IDirect3DDevice9Ex* device, int width, int height)
				{
					SharedSurface.Create(device, width, height);
				}

				/*
					This is the surface that we draw on to.
					It is shared with a DirectX 11 texture so we can run it through shaders.
				*/
				SharedSurfaceData SharedSurface;
			} DirectX9;

			struct DirectX11Data
			{
				/*
					Base for hardware conversion routines.
				*/
				struct ConversionBase
				{
					virtual ~ConversionBase() = default;

					virtual void Create(ID3D11Device* device, AVFrame* reference) = 0;

					/*
						States that need update every frame.
					*/
					virtual void DynamicBind(ID3D11DeviceContext* context) = 0;

					/*
						Try to retrieve data to CPU after an operation.
					*/
					virtual bool Download(ID3D11DeviceContext* context, VideoFutureData& item) = 0;
				};

				/*
					Hardware conversion shaders will store their data in this type.
					It's readable by the CPU and the finished frame is expected to be in
					the right format.
				*/
				struct GPUBuffer
				{
					void Create(ID3D11Device* device, DXGI_FORMAT viewformat, int size, int numelements)
					{
						Staging = UseStaging();

						/*
							Staging requires two buffers, one that the GPU operates on and then
							copies into another buffer that the CPU can read.
						*/
						if (Staging)
						{
							D3D11_BUFFER_DESC desc = {};
							desc.ByteWidth = size;
							desc.Usage = D3D11_USAGE_DEFAULT;
							desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

							SDR::Error::MS::ThrowIfFailed
							(
								device->CreateBuffer(&desc, nullptr, Buffer.GetAddressOf()),
								"Could not create generic GPU buffer for staging"
							);

							desc.BindFlags = 0;
							desc.Usage = D3D11_USAGE_STAGING;
							desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

							SDR::Error::MS::ThrowIfFailed
							(
								device->CreateBuffer(&desc, nullptr, BufferStaging.GetAddressOf()),
								"Could not create staging GPU read buffer"
							);
						}

						/*
							Other method only requires a single buffer that can be read by the CPU and written by the GPU.
						*/
						else
						{
							D3D11_BUFFER_DESC desc = {};
							desc.ByteWidth = size;
							desc.Usage = D3D11_USAGE_DEFAULT;
							desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
							desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

							SDR::Error::MS::ThrowIfFailed
							(
								device->CreateBuffer(&desc, nullptr, Buffer.GetAddressOf()),
								"Could not create generic GPU read buffer"
							);
						}

						D3D11_UNORDERED_ACCESS_VIEW_DESC viewdesc = {};
						viewdesc.Format = viewformat;
						viewdesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
						viewdesc.Buffer.NumElements = numelements;

						SDR::Error::MS::ThrowIfFailed
						(
							device->CreateUnorderedAccessView(Buffer.Get(), &viewdesc, View.GetAddressOf()),
							"Could not create UAV for generic GPU read buffer"
						);
					}

					HRESULT Map(ID3D11DeviceContext* context, D3D11_MAPPED_SUBRESOURCE* mapped)
					{
						if (Staging)
						{
							context->CopyResource(BufferStaging.Get(), Buffer.Get());
							return context->Map(BufferStaging.Get(), 0, D3D11_MAP_READ, 0, mapped);
						}

						return context->Map(Buffer.Get(), 0, D3D11_MAP_READ, 0, mapped);
					}

					void Unmap(ID3D11DeviceContext* context)
					{
						if (Staging)
						{
							context->Unmap(BufferStaging.Get(), 0);
							return;
						}

						context->Unmap(Buffer.Get(), 0);
					}

					bool Staging;
					Microsoft::WRL::ComPtr<ID3D11Buffer> Buffer;
					Microsoft::WRL::ComPtr<ID3D11Buffer> BufferStaging;
					Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> View;
				};

				struct ConversionBGR0 : ConversionBase
				{
					virtual void Create(ID3D11Device* device, AVFrame* reference) override
					{
						auto size = reference->buf[0]->size;
						auto count = size / sizeof(uint32_t);

						Buffer.Create(device, DXGI_FORMAT_R32_UINT, size, count);
					}

					virtual void DynamicBind(ID3D11DeviceContext* context) override
					{
						auto uavs = { Buffer.View.Get() };
						context->CSSetUnorderedAccessViews(0, 1, uavs.begin(), nullptr);
					}

					virtual bool Download(ID3D11DeviceContext* context, VideoFutureData& item) override
					{
						Profile::ScopedEntry e1(Profile::Types::PushRGB);

						D3D11_MAPPED_SUBRESOURCE mapped;

						auto hr = Buffer.Map(context, &mapped);

						if (FAILED(hr))
						{
							SDR::Log::Warning("SDR: Could not map DX11 RGB buffer\n"s);
						}

						else
						{
							auto ptr = (uint8_t*)mapped.pData;
							item.Planes[0].assign(ptr, ptr + mapped.RowPitch);
						}

						Buffer.Unmap(context);

						return SUCCEEDED(hr);
					}

					GPUBuffer Buffer;
				};

				struct ConversionYUV : ConversionBase
				{
					virtual void Create(ID3D11Device* device, AVFrame* reference) override
					{
						auto sizey = reference->buf[0]->size;
						auto sizeu = reference->buf[1]->size;
						auto sizev = reference->buf[2]->size;

						Y.Create(device, DXGI_FORMAT_R8_UINT, sizey, sizey);
						U.Create(device, DXGI_FORMAT_R8_UINT, sizeu, sizeu);
						V.Create(device, DXGI_FORMAT_R8_UINT, sizev, sizev);

						/*
							Matches YUVInputData in YUVShared.hlsl.
						*/
						__declspec(align(16)) struct
						{
							int Strides[3];
							int Padding1;
							float CoeffY[3];
							int Padding2;
							float CoeffU[3];
							int Padding3;
							float CoeffV[3];
						} yuvdata;

						yuvdata.Strides[0] = reference->linesize[0];
						yuvdata.Strides[1] = reference->linesize[1];
						yuvdata.Strides[2] = reference->linesize[2];

						auto setcoeffs = [](auto& obj, float x, float y, float z)
						{
							obj[0] = x;
							obj[1] = y;
							obj[2] = z;
						};

						/*
							https://msdn.microsoft.com/en-us/library/windows/desktop/ms698715.aspx
						*/

						if (reference->colorspace == AVCOL_SPC_BT470BG)
						{
							setcoeffs(yuvdata.CoeffY, +0.299000, +0.587000, +0.114000);
							setcoeffs(yuvdata.CoeffU, -0.168736, -0.331264, +0.500000);
							setcoeffs(yuvdata.CoeffV, +0.500000, -0.418688, -0.081312);
						}

						else if (reference->colorspace == AVCOL_SPC_BT709)
						{
							setcoeffs(yuvdata.CoeffY, +0.212600, +0.715200, +0.072200);
							setcoeffs(yuvdata.CoeffU, -0.114572, -0.385428, +0.500000);
							setcoeffs(yuvdata.CoeffV, +0.500000, -0.454153, -0.045847);
						}

						else
						{
							SDR::Error::Make("No matching YUV color space for coefficients");
						}

						D3D11_BUFFER_DESC cbufdesc = {};
						cbufdesc.ByteWidth = sizeof(yuvdata);
						cbufdesc.Usage = D3D11_USAGE_IMMUTABLE;
						cbufdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

						D3D11_SUBRESOURCE_DATA cbufsubdesc = {};
						cbufsubdesc.pSysMem = &yuvdata;

						SDR::Error::MS::ThrowIfFailed
						(
							device->CreateBuffer(&cbufdesc, &cbufsubdesc, ConstantBuffer.GetAddressOf()),
							"Could not create constant buffer for YUV GPU buffer"
						);
					}

					virtual void DynamicBind(ID3D11DeviceContext* context) override
					{
						auto cbufs = { ConstantBuffer.Get() };
						context->CSSetConstantBuffers(1, 1, cbufs.begin());

						auto uavs = { Y.View.Get(), U.View.Get(), V.View.Get() };
						context->CSSetUnorderedAccessViews(0, 3, uavs.begin(), nullptr);
					}

					virtual bool Download(ID3D11DeviceContext* context, VideoFutureData& item) override
					{
						Profile::ScopedEntry e1(Profile::Types::PushYUV);

						D3D11_MAPPED_SUBRESOURCE mappedy;
						D3D11_MAPPED_SUBRESOURCE mappedu;
						D3D11_MAPPED_SUBRESOURCE mappedv;

						auto hrs =
						{
							Y.Map(context, &mappedy),
							U.Map(context, &mappedu),
							V.Map(context, &mappedv)
						};

						bool pass = true;

						for (auto res : hrs)
						{
							if (FAILED(res))
							{
								pass = false;

								SDR::Log::Warning("SDR: Could not map DX11 YUV buffers\n"s);
								break;
							}
						}

						if (pass)
						{
							auto ptry = (uint8_t*)mappedy.pData;
							auto ptru = (uint8_t*)mappedu.pData;
							auto ptrv = (uint8_t*)mappedv.pData;

							item.Planes[0].assign(ptry, ptry + mappedy.RowPitch);
							item.Planes[1].assign(ptru, ptru + mappedu.RowPitch);
							item.Planes[2].assign(ptrv, ptrv + mappedv.RowPitch);
						}

						Y.Unmap(context);
						U.Unmap(context);
						V.Unmap(context);

						return pass;
					}

					GPUBuffer Y;
					GPUBuffer U;
					GPUBuffer V;

					Microsoft::WRL::ComPtr<ID3D11Buffer> ConstantBuffer;
				};

				void Create(ID3D11Device* device, HANDLE dx9handle, AVFrame* reference)
				{
					Microsoft::WRL::ComPtr<ID3D11Resource> tempresource;

					SDR::Error::MS::ThrowIfFailed
					(
						device->OpenSharedResource(dx9handle, IID_PPV_ARGS(tempresource.GetAddressOf())),
						"Could not open shared D3D9 resource"
					);

					SDR::Error::MS::ThrowIfFailed
					(
						tempresource.As(&SharedTexture),
						"Could not query shared D3D9 resource as a D3D11 2D texture"
					);

					SDR::Error::MS::ThrowIfFailed
					(
						device->CreateShaderResourceView(SharedTexture.Get(), nullptr, SharedTextureSRV.GetAddressOf()),
						"Could not create SRV for D3D11 backbuffer texture"
					);

					{
						/*
							As seen in SharedAll.hlsl.
						*/
						struct WorkBufferData
						{
							float Color[3];
							float Padding;
						};

						auto px = reference->width * reference->height;
						auto size = sizeof(WorkBufferData);

						D3D11_BUFFER_DESC bufdesc = {};
						bufdesc.ByteWidth = px * size;
						bufdesc.Usage = D3D11_USAGE_DEFAULT;
						bufdesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
						bufdesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
						bufdesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
						bufdesc.StructureByteStride = size;

						SDR::Error::MS::ThrowIfFailed
						(
							device->CreateBuffer(&bufdesc, nullptr, WorkBuffer.GetAddressOf()),
							"Could not create GPU work buffer"
						);

						D3D11_UNORDERED_ACCESS_VIEW_DESC uavdesc = {};
						uavdesc.Format = DXGI_FORMAT_UNKNOWN;
						uavdesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
						uavdesc.Buffer.NumElements = px;

						SDR::Error::MS::ThrowIfFailed
						(
							device->CreateUnorderedAccessView(WorkBuffer.Get(), &uavdesc, WorkBufferUAV.GetAddressOf()),
							"Could not create UAV for GPU work buffer"
						);

						D3D11_SHADER_RESOURCE_VIEW_DESC srvdesc = {};
						srvdesc.Format = DXGI_FORMAT_UNKNOWN;
						srvdesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
						srvdesc.Buffer.NumElements = px;

						SDR::Error::MS::ThrowIfFailed
						(
							device->CreateShaderResourceView(WorkBuffer.Get(), &srvdesc, WorkBufferSRV.GetAddressOf()),
							"Could not create SRV for GPU work buffer"
						);
					}

					struct RuleData
					{
						using FactoryType = std::function<std::unique_ptr<ConversionBase>()>;

						RuleData
						(
							AVPixelFormat format,
							const char* name,
							const BYTE* data,
							size_t datasize,
							const FactoryType& factory
						) :
							Format(format),
							ShaderName(name),
							Data(data),
							DataSize(datasize),
							Factory(factory)
						{

						}

						bool Matches(const AVFrame* ref) const
						{
							return ref->format == Format;
						}

						AVPixelFormat Format;
						const BYTE* Data;
						size_t DataSize;
						const char* ShaderName;
						FactoryType Factory;
					};

					auto yuvfactory = []()
					{
						return std::make_unique<ConversionYUV>();
					};

					auto bgr0factory = []()
					{
						return std::make_unique<ConversionBGR0>();
					};

					RuleData table[] =
					{
						RuleData(AV_PIX_FMT_YUV420P, "YUV420", CSBlob_YUV420, sizeof(CSBlob_YUV420), yuvfactory),
						RuleData(AV_PIX_FMT_YUV444P, "YUV444", CSBlob_YUV444, sizeof(CSBlob_YUV444), yuvfactory),
						RuleData(AV_PIX_FMT_BGR0, "BGR0", CSBlob_BGR0, sizeof(CSBlob_BGR0), bgr0factory),
					};

					RuleData* found = nullptr;

					for (auto&& entry : table)
					{
						if (entry.Matches(reference))
						{
							found = &entry;
							break;
						}
					}

					if (!found)
					{
						auto name = av_get_pix_fmt_name((AVPixelFormat)reference->format);
						SDR::Error::Make("No conversion rule found for \"%s\"", name);
					}

					OpenShader(device, found->ShaderName, found->Data, found->DataSize, ConversionShader.GetAddressOf());

					ConversionPtr = found->Factory();
					ConversionPtr->Create(device, reference);
				}

				/*
					Between CS dispatches the resources should be unbound.
				*/
				void ResetShaderInputs(ID3D11DeviceContext* context)
				{
					/*
						At most, 3 slots are used for the YUV buffers.
					*/
					const auto count = 3;

					ID3D11ShaderResourceView* srvs[count] = {};
					ID3D11UnorderedAccessView* uavs[count] = {};
					ID3D11Buffer* cbufs[count] = {};

					context->CSSetShaderResources(0, count, srvs);
					context->CSSetUnorderedAccessViews(0, count, uavs, nullptr);
					context->CSSetConstantBuffers(0, count, cbufs);
				}

				/*
					Weighs a new engine frame onto the existing work buffer.
				*/
				void NewFrame(VideoStreamSharedData& shared, float weight)
				{
					auto context = shared.DirectX11.Context.Get();

					auto srvs = { SharedTextureSRV.Get() };
					context->CSSetShaderResources(0, 1, srvs.begin());

					auto uavs = { WorkBufferUAV.Get() };
					context->CSSetUnorderedAccessViews(0, 1, uavs.begin(), nullptr);

					if (shared.DirectX11.SamplingConstantData.Weight != weight)
					{
						D3D11_MAPPED_SUBRESOURCE mapped;

						auto hr = context->Map
						(
							shared.DirectX11.SamplingConstantBuffer.Get(),
							0,
							D3D11_MAP_WRITE_DISCARD,
							0,
							&mapped
						);

						if (FAILED(hr))
						{
							SDR::Log::Warning("SDR: Could not map sampling constant buffer\n"s);
						}

						else
						{
							shared.DirectX11.SamplingConstantData.Weight = weight;

							std::memcpy
							(
								mapped.pData,
								&shared.DirectX11.SamplingConstantData,
								sizeof(shared.DirectX11.SamplingConstantData)
							);
						}

						context->Unmap(shared.DirectX11.SamplingConstantBuffer.Get(), 0);
					}

					auto cbufs =
					{
						shared.DirectX11.SharedConstantBuffer.Get(),
						shared.DirectX11.SamplingConstantBuffer.Get()
					};

					context->CSSetConstantBuffers(0, 2, cbufs.begin());

					context->CSSetShader(shared.DirectX11.SamplingShader.Get(), nullptr, 0);

					Dispatch(shared);

					/*
						Force processing right now. If this flush is not here
						then the queue will clear and only transmit the latest frame
						at the GPU -> CPU sync point which effectively disables the entire
						sampling effect.
					*/

					context->Flush();

					ResetShaderInputs(context);
				}

				/*
					Clears the work buffer to black color.
				*/
				void Clear(VideoStreamSharedData& shared)
				{
					auto context = shared.DirectX11.Context.Get();

					context->CSSetShader(shared.DirectX11.ClearShader.Get(), nullptr, 0);

					auto uavs = { WorkBufferUAV.Get() };
					context->CSSetUnorderedAccessViews(0, 1, uavs.begin(), nullptr);

					auto cbufs = { shared.DirectX11.SharedConstantBuffer.Get() };
					context->CSSetConstantBuffers(0, 1, cbufs.begin());

					Dispatch(shared);

					ResetShaderInputs(context);
				}

				/*
					Pass the latest engine frame directly into the work buffer.
				*/
				void Pass(VideoStreamSharedData& shared)
				{
					auto context = shared.DirectX11.Context.Get();

					context->CSSetShader(shared.DirectX11.PassShader.Get(), nullptr, 0);

					auto srvs = { SharedTextureSRV.Get() };
					context->CSSetShaderResources(0, 1, srvs.begin());

					auto uavs = { WorkBufferUAV.Get() };
					context->CSSetUnorderedAccessViews(0, 1, uavs.begin(), nullptr);

					auto cbufs = { shared.DirectX11.SharedConstantBuffer.Get() };
					context->CSSetConstantBuffers(0, 1, cbufs.begin());

					Dispatch(shared);

					ResetShaderInputs(context);
				}

				/*
					Converts to user format.
				*/
				void Conversion(VideoStreamSharedData& shared)
				{
					auto context = shared.DirectX11.Context.Get();

					context->CSSetShader(ConversionShader.Get(), nullptr, 0);

					auto srvs = { WorkBufferSRV.Get() };
					context->CSSetShaderResources(0, 1, srvs.begin());

					auto cbufs = { shared.DirectX11.SharedConstantBuffer.Get() };
					context->CSSetConstantBuffers(0, 1, cbufs.begin());

					ConversionPtr->DynamicBind(context);

					Dispatch(shared);

					ResetShaderInputs(context);
				}

				bool Download(VideoStreamSharedData& shared, VideoFutureData& item)
				{
					auto context = shared.DirectX11.Context.Get();
					return ConversionPtr->Download(context, item);
				}

				void Dispatch(const VideoStreamSharedData& shared)
				{
					auto& dx11 = shared.DirectX11;
					dx11.Context->Dispatch(dx11.GroupsX, dx11.GroupsY, 1);
				}

				/*
					The newest and freshest frame provided by the engine.
				*/
				Microsoft::WRL::ComPtr<ID3D11Texture2D> SharedTexture;
				Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> SharedTextureSRV;

				/*
					Format specific buffer for format conversions. Handles
					binding shader resources and downloading the finished frame.
				*/
				std::unique_ptr<ConversionBase> ConversionPtr;

				/*
					Varying shader, handled by FrameBuffer. Using frame data from WorkBuffer,
					this shader will write into the varying bound resources.
				*/
				Microsoft::WRL::ComPtr<ID3D11ComputeShader> ConversionShader;

				/*
					Data that will be sent off for conversion. This buffer is of type
					WorkBufferData both on the CPU and GPU.
				*/
				Microsoft::WRL::ComPtr<ID3D11Buffer> WorkBuffer;
				Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> WorkBufferUAV;
				Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> WorkBufferSRV;
			} DirectX11;

			SDRVideoWriter Video;

			/*
				Skip first frame as it will always be black when capturing the engine backbuffer.
			*/
			bool FirstFrame = true;

			struct
			{
				double Remainder = 0;
			} SamplingData;
		};

		struct
		{
			bool Enabled;
			float Exposure;

			double TimePerSample;
			double TimePerFrame;
		} SamplingData;

		std::unique_ptr<VideoStreamBase> VideoStream;

		std::thread FrameBufferThreadHandle;
		std::unique_ptr<VideoQueueType> VideoQueue;
	};

	MovieData CurrentMovie;
	std::atomic_int32_t BufferedFrames;
	std::atomic_bool ShouldStopFrameThread;
	std::atomic_bool IsStoppingAsync;

	void SDR_TryJoinFrameThread()
	{
		if (!ShouldStopFrameThread)
		{
			ShouldStopFrameThread = true;
			CurrentMovie.FrameBufferThreadHandle.join();
		}
	}

	bool SDR_ShouldRecord()
	{
		if (!CurrentMovie.IsStarted)
		{
			return false;
		}

		if (SDR::SourceGlobals::IsDrawingLoading())
		{
			return false;
		}

		if (SDR::EngineClient::IsConsoleVisible())
		{
			return false;
		}

		return true;
	}

	void FrameBufferThread()
	{
		auto& movie = CurrentMovie;

		MovieData::VideoFutureData item;

		while (!ShouldStopFrameThread)
		{
			while (movie.VideoQueue->try_dequeue(item))
			{
				--BufferedFrames;

				item.Writer->SetFrameInput(item.Planes);
				item.Writer->SendRawFrame();
			}
		}
	}
}

namespace
{
	namespace ModuleView_Render
	{
		namespace Common
		{
			bool CopyDX9ToDX11(MovieData::VideoStreamBase* stream)
			{
				HRESULT hr;
				Microsoft::WRL::ComPtr<IDirect3DSurface9> surface;

				auto dx9device = SDR::SourceGlobals::GetD3D9DeviceEx();

				hr = dx9device->GetRenderTarget(0, surface.GetAddressOf());

				if (FAILED(hr))
				{
					SDR::Log::Warning("SDR: Could not get DX9 RT\n"s);
					return false;
				}

				/*
					The DX11 texture now contains this data.
				*/
				hr = dx9device->StretchRect
				(
					surface.Get(),
					nullptr,
					stream->DirectX9.SharedSurface.Surface.Get(),
					nullptr,
					D3DTEXF_NONE
				);

				if (FAILED(hr))
				{
					SDR::Log::Warning("SDR: Could not copy DX9 RT -> DX11 RT\n"s);
					return false;
				}

				return true;
			}

			void Pass(MovieData::VideoStreamBase* stream)
			{
				if (!CopyDX9ToDX11(stream))
				{
					return;
				}

				auto& sampling = CurrentMovie.SamplingData;

				auto save = [=]()
				{
					MovieData::VideoFutureData item;
					item.Writer = &stream->Video;

					stream->DirectX11.Conversion(CurrentMovie.VideoStreamShared);
					auto res = stream->DirectX11.Download(CurrentMovie.VideoStreamShared, item);

					if (res)
					{
						++BufferedFrames;
						CurrentMovie.VideoQueue->enqueue(std::move(item));
					}
				};

				/*
					When enough frames have been sampled to form a total weight of 1, it will print the final frame.
				*/
				if (sampling.Enabled)
				{
					auto proc = [=](float weight)
					{
						auto& shared = CurrentMovie.VideoStreamShared;
						stream->DirectX11.NewFrame(shared, weight);
					};

					auto clear = [=]()
					{
						stream->DirectX11.Clear(CurrentMovie.VideoStreamShared);
					};

					auto& rem = stream->SamplingData.Remainder;
					auto oldrem = rem;
					auto exposure = sampling.Exposure;

					rem += sampling.TimePerSample / sampling.TimePerFrame;

					/*
						Cast to float to prevent comparisons against 0.99999999998...
					*/

					if ((float)rem <= (1.0 - exposure))
					{

					}

					else if ((float)rem < 1.0)
					{
						auto weight = (rem - std::max(1.0 - exposure, oldrem)) * (1.0 / exposure);
						proc(weight);
					}

					else
					{
						auto weight = (1.0 - std::max(1.0 - exposure, oldrem)) * (1.0 / exposure);

						proc(weight);
						save();

						rem -= 1.0;

						uint32_t additional = rem;

						if (additional > 0)
						{
							for (int i = 0; i < additional; i++)
							{
								save();
							}

							rem -= additional;
						}

						clear();

						if (rem > FLT_EPSILON && rem > (1.0 - exposure))
						{
							weight = ((rem - (1.0 - exposure)) * (1.0 / exposure));
							proc(weight);
						}
					}
				}

				/*
					No sampling, just pass through as is to conversion shader and save.
				*/
				else
				{
					stream->DirectX11.Pass(CurrentMovie.VideoStreamShared);
					save();
				}
			}

			void Procedure()
			{
				auto& movie = CurrentMovie;
				bool dopasses = SDR_ShouldRecord();

				if (dopasses)
				{
					/*
						Don't risk running out of memory. Just let the encoding finish so we start fresh with no buffered frames.
					*/
					if (MovieData::WouldNewFrameOverflow())
					{
						while (BufferedFrames)
						{
							std::this_thread::sleep_for(1ms);
						}
					}

					if (movie.VideoStream->FirstFrame)
					{
						movie.VideoStream->FirstFrame = false;
						CopyDX9ToDX11(movie.VideoStream.get());
					}

					else
					{
						Pass(movie.VideoStream.get());
					}
				}
			}
		}

		enum
		{
			VariantCount = 1
		};

		namespace Variant0
		{
			void __fastcall NewFunction(void* thisptr, void* edx, void* rect);

			using OverrideType = decltype(NewFunction)*;
			SDR::HookModule<OverrideType> ThisHook;

			void __fastcall NewFunction(void* thisptr, void* edx, void* rect)
			{
				ThisHook.GetOriginal()(thisptr, edx, rect);
				Common::Procedure();
			}
		}

		auto Adders = SDR::CreateAdders
		(
			SDR::ModuleHandlerAdder
			(
				"View_Render",
				[](const char* name, rapidjson::Value& value)
				{
					return SDR::GenericHookVariantInit
					(
						{SDR::GenericHookInitParam(Variant0::ThisHook, Variant0::NewFunction)},
						name,
						value
					);
				}
			)
		);
	}

	namespace ModuleStartMovie
	{
		namespace Common
		{
			void VerifyOutputDirectory(const char* path)
			{
				char final[SDR::File::NameSize];
				strcpy_s(final, path);

				PathAddBackslashA(final);

				auto winstr = SDR::String::FromUTF8(final);

				auto res = PathFileExistsW(winstr.c_str()) == 1;

				if (!res)
				{
					auto error = GetLastError();
					auto hr = HRESULT_FROM_WIN32(error);

					SDR::Error::MS::ThrowIfFailed(hr, "Could not access wanted output directory");
				}
			}

			std::string BuildVideoStreamName(const char* savepath, const char* filename)
			{
				char finalname[SDR::File::NameSize];

				PathCombineA(finalname, savepath, filename);

				return {finalname};
			}

			void WarnAboutVariableValues()
			{
				auto newstr = Variables::Video::Encoder.GetString();
				auto encoder = avcodec_find_encoder_by_name(newstr);

				if (!encoder)
				{
					SDR::Log::Warning("SDR: Encoder \"%s\" not found, available encoders:\n", newstr);

					auto next = av_codec_next(nullptr);

					while (next)
					{
						SDR::Log::Message("SDR: %s\n", next->name);
						next = av_codec_next(next);
					}
				}

				else
				{
					if (encoder->id == AV_CODEC_ID_H264)
					{
						auto newstr = Variables::Video::X264::Preset.GetString();

						auto slowpresets =
						{
							"slow",
							"slower",
							"veryslow",
							"placebo"
						};

						for (auto preset : slowpresets)
						{
							if (_strcmpi(newstr, preset) == 0)
							{
								SDR::Log::Warning("SDR: Slow encoder preset chosen, this might not work very well for realtime\n"s);
								break;
							}
						}
					}
				}
			}

			void Procedure(const char* filename, int width, int height)
			{
				CurrentMovie = {};

				auto& movie = CurrentMovie;

				WarnAboutVariableValues();

				try
				{
					auto sdrpath = Variables::OutputDirectory.GetString();

					/*
						No desired path, use game root.
					*/
					if (strlen(sdrpath) == 0)
					{
						sdrpath = SDR::GetGamePath();
					}

					else
					{
						VerifyOutputDirectory(sdrpath);
					}

					auto linktabletovariable = [](const char* key, const auto& table, auto& variable)
					{
						for (const auto& entry : table)
						{
							if (_strcmpi(key, entry.first) == 0)
							{
								variable = entry.second;
								return true;
							}
						}

						return false;
					};

					/*
						Default to 709 space and full range.
					*/
					auto colorspace = AVCOL_SPC_BT709;
					auto colorrange = AVCOL_RANGE_JPEG;
					auto pxformat = AV_PIX_FMT_NONE;

					{
						auto table =
						{
							std::make_pair("601", AVCOL_SPC_BT470BG),
							std::make_pair("709", AVCOL_SPC_BT709)
						};

						linktabletovariable(Variables::Video::ColorSpace.GetString(), table, colorspace);
					}

					av_log_set_callback(LAV::LogFunction);

					auto stream = std::make_unique<MovieData::VideoStreamBase>();

					struct VideoConfigurationData
					{
						using FormatsType = std::vector<std::pair<const char*, AVPixelFormat>>;

						VideoConfigurationData(const char* name, FormatsType&& formats) :
							Encoder(avcodec_find_encoder_by_name(name)),
							PixelFormats(std::move(formats))
						{

						}

						AVCodec* Encoder;
						FormatsType PixelFormats;
					};

					const auto yuv420 = std::make_pair("yuv420", AV_PIX_FMT_YUV420P);
					const auto yuv444 = std::make_pair("yuv444", AV_PIX_FMT_YUV444P);
					const auto bgr0 = std::make_pair("bgr0", AV_PIX_FMT_BGR0);

					VideoConfigurationData table[] =
					{
						VideoConfigurationData("libx264", { yuv420, yuv444 }),
						VideoConfigurationData("libx264rgb", { bgr0 }),
					};

					const VideoConfigurationData* vidconfig = nullptr;

					{
						auto encoderstr = Variables::Video::Encoder.GetString();
						auto encoder = avcodec_find_encoder_by_name(encoderstr);

						SDR::Error::ThrowIfNull(encoder, "Video encoder \"%s\" not found", encoderstr);

						for (const auto& config : table)
						{
							if (config.Encoder == encoder)
							{
								vidconfig = &config;
								break;
							}
						}

						auto pxformatstr = Variables::Video::PixelFormat.GetString();

						if (!linktabletovariable(pxformatstr, vidconfig->PixelFormats, pxformat))
						{
							/*
								User selected pixel format does not match any in config.
							*/
							pxformat = vidconfig->PixelFormats[0].second;
						}

						auto isrgbtype = [](AVPixelFormat format)
						{
							auto table =
							{
								AV_PIX_FMT_BGR0,
							};

							for (auto entry : table)
							{
								if (format == entry)
								{
									return true;
								}
							}

							return false;
						};

						if (isrgbtype(pxformat))
						{
							colorspace = AVCOL_SPC_RGB;
							colorrange = AVCOL_RANGE_UNSPECIFIED;
						}

						stream->Video.Frame.Assign(width, height, pxformat, colorspace, colorrange);

						movie.VideoStreamShared.DirectX11.Create(width, height);

						stream->DirectX9.Create(SDR::SourceGlobals::GetD3D9DeviceEx(), width, height);

						stream->DirectX11.Create
						(
							movie.VideoStreamShared.DirectX11.Device.Get(),
							stream->DirectX9.SharedSurface.SharedHandle,
							stream->Video.Frame.Get()
						);

						/*
							Destroy any deferred D3D11 resources created by above functions.
						*/
						movie.VideoStreamShared.DirectX11.Context->Flush();

						auto name = BuildVideoStreamName(sdrpath, filename);

						stream->Video.OpenFileForWrite(name.c_str());
						stream->Video.SetEncoder(vidconfig->Encoder);
					}

					{
						LAV::ScopedAVDictionary options;

						if (vidconfig->Encoder->id == AV_CODEC_ID_H264)
						{
							namespace X264 = Variables::Video::X264;

							auto preset = X264::Preset.GetString();
							auto crf = X264::CRF.GetString();
							auto intra = X264::Intra.GetBool();

							options.Set("preset", preset);
							options.Set("crf", crf);

							if (intra)
							{
								/*
									Setting every frame as a keyframe gives the ability to use the video in a video editor with ease.
								*/
								options.Set("x264-params", "keyint=1");
							}
						}

						auto fps = Variables::Video::Framerate.GetInt();
						stream->Video.OpenEncoder(fps, options.Get());

						stream->Video.WriteHeader();
					}

					/*
						All went well, move state over.
					*/
					movie.VideoStream = std::move(stream);
				}

				catch (const SDR::Error::Exception& error)
				{
					CurrentMovie = {};
					return;
				}

				/*
					Don't call the original CL_StartMovie as it causes major recording slowdowns.
				*/

				auto fps = Variables::Video::Framerate.GetInt();
				auto exposure = Variables::Video::Sample::Exposure.GetFloat();
				auto mult = Variables::Video::Sample::Multiply.GetInt();

				auto enginerate = fps;

				movie.SamplingData.Enabled = MovieData::UseSampling();

				if (movie.SamplingData.Enabled)
				{
					enginerate *= mult;

					movie.SamplingData.Exposure = exposure;
					movie.SamplingData.TimePerSample = 1.0 / enginerate;
					movie.SamplingData.TimePerFrame = 1.0 / fps;
				}

				SDR::Console::Variable hostframerate("host_framerate");
				hostframerate.SetValue(enginerate);

				SDR::Console::Variable matqueuemode("mat_queue_mode");
				movie.OldMatQueueModeValue = matqueuemode.GetInt();

				/*
					Force single threaded processing or else there will be flickering.
				*/
				matqueuemode.SetValue(0);

				/*
					Make room for some entries in the queues.
				*/
				movie.VideoQueue = std::make_unique<MovieData::VideoQueueType>(256);

				movie.IsStarted = true;
				BufferedFrames = 0;
				ShouldStopFrameThread = false;
				IsStoppingAsync = false;

				movie.FrameBufferThreadHandle = std::thread(FrameBufferThread);

				SDR::Log::Message("SDR: Started movie\n"s);
			}
		}

		enum
		{
			VariantCount = 1
		};

		namespace Variant0
		{
			/*
				The 7th parameter (unk) was been added in Source 2013, it's not there in Source 2007.
			*/
			void __cdecl NewFunction(const char* filename, int flags, int width, int height, float framerate, int jpegquality, int unk);

			using OverrideType = decltype(NewFunction)*;
			SDR::HookModule<OverrideType> ThisHook;

			void __cdecl NewFunction(const char* filename, int flags, int width, int height, float framerate, int jpegquality, int unk)
			{
				Common::Procedure(filename, width, height);
			}
		}

		auto Adders = SDR::CreateAdders
		(
			SDR::ModuleHandlerAdder
			(
				"StartMovie",
				[](const char* name, rapidjson::Value& value)
				{
					return SDR::GenericHookVariantInit
					(
						{SDR::GenericHookInitParam(Variant0::ThisHook, Variant0::NewFunction)},
						name,
						value
					);
				}
			)
		);
	}

	namespace ModuleStartMovieCommand
	{
		namespace Common
		{
			/*
				This command is overriden to remove the incorrect description.
			*/
			void Procedure(const void* ptr)
			{
				SDR::Console::CommandArgs args(ptr);

				if (CurrentMovie.IsStarted)
				{
					SDR::Log::Message("SDR: Movie is already started\n"s);
					return;
				}

				if (args.Count() < 2)
				{
					SDR::Log::Message("SDR: Name is required for startmovie, see Github page for help\n"s);
					return;
				}

				int width;
				int height;

				if (!SDR::MaterialSystem::GetBackBufferDimensions(width, height))
				{
					SDR::Log::Warning("SDR: Could not get backbuffer dimensions\n"s);
					return;
				}

				Profile::Reset();

				/*
					Retrieve everything after the initial "startmovie" token, in case
					of special UTF8 names the ArgV is split.
				*/
				auto name = args.FullArgs();

				while (true)
				{
					++name;

					if (*name == ' ')
					{
						++name;
						break;
					}
				}

				ModuleStartMovie::Common::Procedure(name, width, height);
			}
		}

		enum
		{
			VariantCount = 1
		};

		namespace Variant0
		{
			void __cdecl NewFunction(const void* ptr);

			using OverrideType = decltype(NewFunction)*;
			SDR::HookModule<OverrideType> ThisHook;

			void __cdecl NewFunction(const void* ptr)
			{
				Common::Procedure(ptr);
			}
		}

		auto Adders = SDR::CreateAdders
		(
			SDR::ModuleHandlerAdder
			(
				"StartMovieCommand",
				[](const char* name, rapidjson::Value& value)
				{
					return SDR::GenericHookVariantInit
					(
						{SDR::GenericHookInitParam(Variant0::ThisHook, Variant0::NewFunction)},
						name,
						value
					);
				}
			)
		);
	}

	namespace ModuleEndMovie
	{
		namespace Common
		{
			void Procedure(bool async)
			{
				if (!CurrentMovie.IsStarted)
				{
					SDR::Log::Message("SDR: No movie is started\n"s);
					return;
				}

				CurrentMovie.IsStarted = false;

				/*
					Don't call original function as we don't call the engine's startmovie.
				*/

				SDR::Console::Variable hostframerate("host_framerate");
				hostframerate.SetValue(0);

				SDR::Console::Variable matqueuemode("mat_queue_mode");
				matqueuemode.SetValue(CurrentMovie.OldMatQueueModeValue);

				SDR::Log::Message("SDR: Ending movie, if there are buffered frames this might take a moment\n"s);

				auto func = []()
				{
					SDR_TryJoinFrameThread();

					/*
						Let the encoder finish all the delayed frames.
					*/
					CurrentMovie.VideoStream->Video.Finish();

					CurrentMovie = {};

					if (Variables::ExitOnFinish.GetBool())
					{
						SDR::EngineClient::ClientCommand("quit\n");
						return;
					}

					if (Variables::FlashWindow.GetBool())
					{
						SDR::EngineClient::FlashWindow();
					}

					SDR::Log::MessageColor({ 88, 255, 39 }, "SDR: Movie is now complete\n"s);

					Profile::ShowResults();
				};

				if (async)
				{
					IsStoppingAsync = true;

					auto task = concurrency::create_task(func);

					task.then([]()
					{
						IsStoppingAsync = false;
					});
				}

				else
				{
					func();
				}
			}
		}

		enum
		{
			VariantCount = 1
		};

		namespace Variant0
		{
			void __cdecl NewFunction();

			using OverrideType = decltype(NewFunction)*;
			SDR::HookModule<OverrideType> ThisHook;

			void __cdecl NewFunction()
			{
				Common::Procedure(true);
			}
		}

		auto Adders = SDR::CreateAdders
		(
			SDR::ModuleHandlerAdder
			(
				"EndMovie",
				[](const char* name, rapidjson::Value& value)
				{
					return SDR::GenericHookVariantInit
					(
						{SDR::GenericHookInitParam(Variant0::ThisHook, Variant0::NewFunction)},
						name,
						value
					);
				}
			)
		);
	}

	namespace ModuleEndMovieCommand
	{
		namespace Common
		{
			/*
				Always allow ending movie.
			*/
			void Procedure()
			{
				ModuleEndMovie::Common::Procedure(true);
			}
		}

		enum
		{
			VariantCount = 1
		};

		namespace Variant0
		{
			void __cdecl NewFunction();

			using OverrideType = decltype(NewFunction)*;
			SDR::HookModule<OverrideType> ThisHook;

			void __cdecl NewFunction()
			{
				Common::Procedure();
			}
		}

		auto Adders = SDR::CreateAdders
		(
			SDR::ModuleHandlerAdder
			(
				"EndMovieCommand",
				[](const char* name, rapidjson::Value& value)
				{
					return SDR::GenericHookVariantInit
					(
						{SDR::GenericHookInitParam(Variant0::ThisHook, Variant0::NewFunction)},
						name,
						value
					);
				}
			)
		);
	}

	/*
		This function handles plugin_unload, and in the event that endmovie wasn't called.
		The cleaning up cannot be done asynchronously as the module itself gets unloaded.
	*/
	SDR::PluginShutdownFunctionAdder A1([]()
	{
		if (IsStoppingAsync)
		{
			SDR::Log::Message("SDR: Already stopping asynchronously\n"s);

			while (IsStoppingAsync)
			{
				std::this_thread::sleep_for(1ms);
			}
		}

		ModuleEndMovie::Common::Procedure(false);
	});

	namespace ModuleSUpdateGuts
	{
		enum
		{
			VariantCount = 1
		};

		namespace Variant0
		{
			void __cdecl NewFunction(float mixahead);

			using OverrideType = decltype(NewFunction)*;
			SDR::HookModule<OverrideType> ThisHook;

			void __cdecl NewFunction(float mixahead)
			{
				if (!CurrentMovie.IsStarted)
				{
					ThisHook.GetOriginal()(mixahead);
				}
			}
		}

		auto Adders = SDR::CreateAdders
		(
			SDR::ModuleHandlerAdder
			(
				"SUpdateGuts",
				[](const char* name, rapidjson::Value& value)
				{
					return SDR::GenericHookVariantInit
					(
						{SDR::GenericHookInitParam(Variant0::ThisHook, Variant0::NewFunction)},
						name,
						value
					);
				}
			)
		);
	}
}