#define UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_DEPRECATE

#include <intrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include <initguid.h>

#ifndef NDEBUG
#	define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#	define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)
#else
#	define Assert(cond) (void)(cond)
#	define HR(hr) do { HRESULT _hr = (hr); } while (0)
#endif

#define DERPNET_USE_PLAIN_HTTP 0
#define DERPNET_STATIC
#include "external/derpnet.h"
#include "external/wcap_screen_capture.h"
#include "external/WindowsJson.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <evr.h>
#include <pathcch.h>
#include <strsafe.h>

#include "ScreenBuddyVS.h"
#include "ScreenBuddyPS.h"

#pragma comment (lib, "kernel32")
#pragma comment (lib, "user32")
#pragma comment (lib, "ole32")
#pragma comment (lib, "gdi32")
#pragma comment (lib, "winhttp")
#pragma comment (lib, "dwmapi")
#pragma comment (lib, "mfplat")
#pragma comment (lib, "mfuuid")
#pragma comment (lib, "strmiids")
#pragma comment (lib, "evr")
#pragma comment (lib, "d3d11")
#pragma comment (lib, "pathcch")
#pragma comment (lib, "OneCore")
#pragma comment (lib, "CoreMessaging")

// why this is not documented anywhere???
DEFINE_GUID(MF_XVP_PLAYBACK_MODE, 0x3c5d293f, 0xad67, 0x4e29, 0xaf, 0x12, 0xcf, 0x3e, 0x23, 0x8a, 0xcc, 0xe9);

#define MF64(Hi,Lo) (((UINT64)Hi << 32) | (Lo))

#define StrFormat(Buffer, ...) _snwprintf(Buffer, _countof(Buffer), __VA_ARGS__)

#define BUDDY_CLASS L"ScreenBuddyClass"
#define BUDDY_TITLE L"Screen Buddy"
#define BUDDY_CONFIG L"Buddy"

enum
{
	BUDDY_CONFIG_MAXPATH	= 32 * 1024,

	// encoder settings
	BUDDY_ENCODE_FRAMERATE	= 30,
	BUDDY_ENCODE_BITRATE	= 4 * 1000 * 1000,
	BUDDY_ENCODE_QUEUE_SIZE = 8,

	// DerpMap limits
	BUDDY_MAX_REGION_COUNT = 256,
	BUDDY_MAX_HOST_LENGTH  = 128,

	// windows message notifications
	BUDDY_WM_BEST_REGION = WM_USER + 1,
	BUDDY_WM_MEDIA_EVENT = WM_USER + 2,
	BUDDY_WM_NET_EVENT =   WM_USER + 3,

	// timer ids
	BUDDY_DISCONNECT_TIMER		= 111,
	BUDDY_UPDATE_TITLE_TIMER	= 222,

	// dialog controls
	BUDDY_ID_SHARE_ICON			= 100,
	BUDDY_ID_SHARE_KEY			= 110,
	BUDDY_ID_SHARE_COPY			= 120,
	BUDDY_ID_SHARE_NEW			= 130,
	BUDDY_ID_SHARE_BUTTON		= 140,
	BUDDY_ID_CONNECT_ICON		= 200,
	BUDDY_ID_CONNECT_KEY		= 210,
	BUDDY_ID_CONNECT_PASTE		= 220,
	BUDDY_ID_CONNECT_BUTTON		= 230,

	// dialog layout
	BUDDY_DIALOG_PADDING		= 4,
	BUDDY_DIALOG_ITEM_HEIGHT	= 14,
	BUDDY_DIALOG_BUTTON_WIDTH	= 60,
	BUDDY_DIALOG_BUTTON_SMALL	= BUDDY_DIALOG_ITEM_HEIGHT,
	BUDDY_DIALOG_KEY_WIDTH		= 268,
	BUDDY_DIALOG_WIDTH			= 350,
	BUDDY_DIALOG_ICON_SIZE		= 42,

	// network packets
	BUDDY_PACKET_VIDEO			= 0,
	BUDDY_PACKET_DISCONNECT		= 1,
	BUDDY_PACKET_MOUSE_MOVE		= 2,
	BUDDY_PACKET_MOUSE_BUTTON	= 3,
	BUDDY_PACKET_MOUSE_WHEEL	= 4,
};

typedef enum
{
	BUDDY_STATE_INITIAL,
	BUDDY_STATE_SHARE_STARTED,
	BUDDY_STATE_SHARING,
	BUDDY_STATE_CONNECTING,
	BUDDY_STATE_CONNECTED,
	BUDDY_STATE_DISCONNECTED,
}
BuddyState;

typedef struct
{
	wchar_t ConfigPath[BUDDY_CONFIG_MAXPATH];
	
	// loaded from config
	uint32_t DerpRegion;
	wchar_t DerpRegions[BUDDY_MAX_REGION_COUNT][BUDDY_MAX_HOST_LENGTH];
	DerpKey MyPrivateKey;
	DerpKey MyPublicKey;

	// windows stuff
	HICON Icon;
	HWND MainWindow;
	HWND DialogWindow;

	// derp stuff
	HANDLE DerpRegionThread;
	DerpKey RemoteKey;
	PTP_WAIT WaitCallback;
	size_t LastReceived;

	// graphics stuff
	ID3D11Device* Device;
	ID3D11DeviceContext* Context;
	IDXGISwapChain1* SwapChain;
	ID3D11ShaderResourceView* InputView;
	ID3D11RenderTargetView* OutputView;
	ID3D11VertexShader* VertexShader;
	ID3D11PixelShader* PixelShader;
	ID3D11Buffer* ConstantBuffer;
	bool InputMipsGenerated;
	int InputWidth;
	int InputHeight;
	int OutputWidth;
	int OutputHeight;

	// media stuff
	IMFTransform* Converter;
	IMFTransform* Codec;

	// encoder stuff
	IMFMediaEventGenerator* Generator;
	IMFAsyncCallback EventCallback;
	uint64_t EncodeFirstTime;
	uint64_t EncodeNextTime;
	bool EncodeWaitingForInput;
	IMFSample* EncodeQueue[BUDDY_ENCODE_QUEUE_SIZE];
	uint32_t EncodeQueueRead;
	uint32_t EncodeQueueWrite;
	IMFVideoSampleAllocatorEx* EncodeSampleAllocator;

	// decoder stuff
	uint32_t DecodeInputExpected;
	IMFMediaBuffer* DecodeInputBuffer;
	IMFSample* DecodeOutputSample;

	ScreenCapture Capture;
	DerpNet Net;

	BuddyState State;
	HINTERNET HttpSession;
	uint64_t Freq;
}
ScreenBuddy;

typedef struct
{
	uint8_t Packet;
	int16_t X;
	int16_t Y;
	int16_t Button;
	int16_t IsDownOrHorizontalWheel;
}
Buddy_MousePacket;

//

static size_t Buddy_DownloadDerpMap(HINTERNET HttpSession, uint8_t* Buffer, size_t BufferMaxSize)
{
	size_t BufferSize = 0;

	HINTERNET HttpConnection = WinHttpConnect(HttpSession, L"login.tailscale.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (HttpConnection)
	{
		HINTERNET HttpRequest = WinHttpOpenRequest(HttpConnection, L"GET", L"/derpmap/default", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
		if (HttpRequest)
		{
			if (WinHttpSendRequest(HttpRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(HttpRequest, NULL))
			{
				DWORD Status = 0;
				DWORD StatusSize = sizeof(Status);

				WinHttpQueryHeaders(
					HttpRequest,
					WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX,
					&Status,
					&StatusSize,
					WINHTTP_NO_HEADER_INDEX);

				if (Status == HTTP_STATUS_OK)
				{
					while (BufferSize < BufferMaxSize)
					{
						DWORD Read;
						if (!WinHttpReadData(HttpRequest, Buffer + BufferSize, (DWORD)(BufferMaxSize - BufferSize), &Read) || Read == 0)
						{
							break;
						}
						BufferSize += Read;
					}
				}
			}
			WinHttpCloseHandle(HttpRequest);
		}
		WinHttpCloseHandle(HttpConnection);
	}

	return BufferSize;
}

static DWORD CALLBACK Buddy_GetBestDerpRegionThread(LPVOID Arg)
{
	ScreenBuddy* Buddy = Arg;

	uint8_t Buffer[64 * 1024];
	size_t BufferSize = Buddy_DownloadDerpMap(Buddy->HttpSession, Buffer, sizeof(Buffer));

	JsonObject* Json = JsonObject_Parse((char*)Buffer, (int)BufferSize);
	JsonObject* Regions = JsonObject_GetObject(Json, JsonCSTR("Regions"));
	JsonIterator* Iterator = JsonObject_GetIterator(Regions);
	if (Iterator)
	{
		do
		{
			JsonObject* Region = JsonIterator_GetValue(Iterator);
			uint32_t RegionId = (uint32_t)JsonObject_GetNumber(Region, JsonCSTR("RegionID"));
			if (RegionId < BUDDY_MAX_REGION_COUNT)
			{
				JsonArray* Nodes = JsonObject_GetArray(Region, JsonCSTR("Nodes"));
				JsonObject* Node = JsonArray_GetObject(Nodes, 0);
				HSTRING NodeHost = JsonObject_GetString(Node, JsonCSTR("HostName"));
				if (NodeHost)
				{
					LPCWSTR HostName = WindowsGetStringRawBuffer(NodeHost, NULL);
					lstrcpynW(Buddy->DerpRegions[RegionId], HostName, ARRAYSIZE(Buddy->DerpRegions[RegionId]));
					WindowsDeleteString(NodeHost);
				}
				JsonRelease(Node);
				JsonRelease(Nodes);
			}
			JsonRelease(Region);
		}
		while (JsonIterator_Next(Iterator));
		JsonRelease(Iterator);
	}
	JsonRelease(Regions);
	JsonRelease(Json);

	ADDRINFOEXW AddressHints =
	{
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
	};

	// resolve all hostnames
	PADDRINFOEXW Addresses[BUDDY_MAX_REGION_COUNT] = { 0 };
	for (size_t RegionIndex = 0; RegionIndex != BUDDY_MAX_REGION_COUNT; RegionIndex++)
	{
		if (Buddy->DerpRegions[RegionIndex][0] == 0)
		{
			continue;
		}

		GetAddrInfoExW(Buddy->DerpRegions[RegionIndex], L"443", NS_ALL, NULL, &AddressHints, &Addresses[RegionIndex], NULL, NULL, NULL, NULL);
	}

	// create nonblocking sockets
	SOCKET Sockets[BUDDY_MAX_REGION_COUNT] = { 0 };
	for (size_t RegionIndex = 0; RegionIndex != BUDDY_MAX_REGION_COUNT; RegionIndex++)
	{
		PADDRINFOEXW Address = Addresses[RegionIndex];
		if (Address == NULL)
		{
			continue;
		}

		SOCKET Socket = socket(Address->ai_family, Address->ai_socktype, Address->ai_protocol);
		Assert(Socket != INVALID_SOCKET);

		u_long NonBlocking = 1;
		int NonBlockingOk = ioctlsocket(Socket, FIONBIO, &NonBlocking);
		Assert(NonBlockingOk == 0);

		Sockets[RegionIndex] = Socket;
	}

	uint32_t BestRegion = 0;

	// start connections
	for (size_t RegionIndex = 0; RegionIndex != BUDDY_MAX_REGION_COUNT; RegionIndex++)
	{
		SOCKET Socket = Sockets[RegionIndex];
		if (Socket == 0)
		{
			continue;
		}

		PADDRINFOEXW Address = Addresses[RegionIndex];
		int Connected = connect(Socket, Address->ai_addr, (int)Address->ai_addrlen);
		if (Connected == 0)
		{
			BestRegion = (uint32_t)RegionIndex;
			break;
		}
		else if (Connected == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			// pending
		}
		else
		{
			Sockets[RegionIndex] = 0;
			closesocket(Socket);
		}
	}

	// wait for first connnection to finish
	if (BestRegion == 0)
	{
		uint32_t PollRegion[BUDDY_MAX_REGION_COUNT];
		WSAPOLLFD Poll[BUDDY_MAX_REGION_COUNT];
		ULONG PollCount = 0;
		for (size_t RegionIndex = 0; RegionIndex != BUDDY_MAX_REGION_COUNT; RegionIndex++)
		{
			if (Sockets[RegionIndex] != 0)
			{
				Poll[PollCount].fd = Sockets[RegionIndex];
				Poll[PollCount].events = POLLOUT;
				PollRegion[PollCount] = (uint32_t)RegionIndex;
				PollCount++;
			}
		}

		if (PollCount && WSAPoll(Poll, PollCount, INFINITE) > 0)
		{
			for (size_t Index = 0; Index != BUDDY_MAX_REGION_COUNT; Index++)
			{
				if (Poll[Index].revents & POLLOUT)
				{
					BestRegion = (uint32_t)PollRegion[Index];
					break;
				}
			}
		}
	}

	// done!
	for (size_t RegionIndex = 0; RegionIndex != BUDDY_MAX_REGION_COUNT; RegionIndex++)
	{
		if (Sockets[RegionIndex])
		{
			closesocket(Sockets[RegionIndex]);
		}
		if (Addresses[RegionIndex])
		{
			FreeAddrInfoExW(Addresses[RegionIndex]);
		}
	}

	PostMessageW(Buddy->DialogWindow, BUDDY_WM_BEST_REGION, BestRegion, 0);

	return 0;
}

static void Buddy_LoadConfig(ScreenBuddy* Buddy)
{
	DWORD ExePathOk = GetModuleFileNameW(NULL, Buddy->ConfigPath, ARRAYSIZE(Buddy->ConfigPath));
	Assert(ExePathOk != 0);

	HR(PathCchRenameExtension(Buddy->ConfigPath, ARRAYSIZE(Buddy->ConfigPath), L".ini"));

	Buddy->DerpRegion = GetPrivateProfileIntW(BUDDY_CONFIG, L"DerpRegion", 0, Buddy->ConfigPath);

	for (int RegionIndex = 0; RegionIndex < BUDDY_MAX_REGION_COUNT; RegionIndex++)
	{
		wchar_t DerpRegionName[128];
		StrFormat(DerpRegionName, L"DerpRegion%d", RegionIndex);

		GetPrivateProfileStringW(BUDDY_CONFIG, DerpRegionName, L"", Buddy->DerpRegions[RegionIndex], ARRAYSIZE(Buddy->DerpRegions[RegionIndex]), Buddy->ConfigPath);
	}

	wchar_t EncryptedText[2048];
	DWORD EncryptedTextLen = GetPrivateProfileStringW(BUDDY_CONFIG, L"DerpPrivateKey", L"", EncryptedText, ARRAYSIZE(EncryptedText), Buddy->ConfigPath);

	bool PrivateKeyValid = false;
	if (EncryptedTextLen)
	{
		uint8_t EncryptedBlob[1024];
		for (size_t i = 0; i < EncryptedTextLen; i += 2)
		{
			swscanf(EncryptedText + i, L"%02hhx", &EncryptedBlob[i/2]);
		}

		DATA_BLOB BlobInput = { EncryptedTextLen, EncryptedBlob };
		DATA_BLOB BlobOutput;
		BOOL Decrypted = CryptUnprotectData(&BlobInput, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &BlobOutput);

		if (Decrypted && BlobOutput.cbData == sizeof(Buddy->MyPrivateKey))
		{
			CopyMemory(&Buddy->MyPrivateKey, BlobOutput.pbData, BlobOutput.cbData);
			PrivateKeyValid = true;
		}

		LocalFree(BlobOutput.pbData);
	}

	if (!PrivateKeyValid)
	{
		DerpNet_CreateNewKey(&Buddy->MyPrivateKey);
	}

	DerpNet_GetPublicKey(&Buddy->MyPrivateKey, &Buddy->MyPublicKey);
}

//

static void CALLBACK Buddy_WaitCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WAIT Wait, TP_WAIT_RESULT WaitResult)
{
	ScreenBuddy* Buddy = Context;
	PostMessageW(Buddy->DialogWindow, BUDDY_WM_NET_EVENT, 0, 0);
}

static void Buddy_NextWait(ScreenBuddy* Buddy)
{
	SetThreadpoolWait(Buddy->WaitCallback, Buddy->Net.SocketEvent, NULL);
}

static void Buddy_StartWait(ScreenBuddy* Buddy)
{
	Buddy->WaitCallback = CreateThreadpoolWait(&Buddy_WaitCallback, Buddy, NULL);
	Assert(Buddy->WaitCallback);

	Buddy_NextWait(Buddy);
}

static void Buddy_CancelWait(ScreenBuddy* Buddy)
{
	SetThreadpoolWait(Buddy->WaitCallback, NULL, NULL);
	WaitForThreadpoolWaitCallbacks(Buddy->WaitCallback, TRUE);
	CloseThreadpoolWait(Buddy->WaitCallback);
}

//

static HRESULT STDMETHODCALLTYPE Buddy__QueryInterface(IMFAsyncCallback* This, REFIID Riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(&IID_IUnknown, Riid) || IsEqualGUID(&IID_IMFAsyncCallback, Riid))
	{
		*Object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Buddy__AddRef(IMFAsyncCallback* This)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE Buddy__Release(IMFAsyncCallback* This)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE Buddy__GetParameters(IMFAsyncCallback* This, DWORD* Flags, DWORD* Queue)
{
	*Flags = 0;
	*Queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Buddy__Invoke(IMFAsyncCallback* This, IMFAsyncResult* AsyncResult)
{
	ScreenBuddy* Buddy = CONTAINING_RECORD(This, ScreenBuddy, EventCallback);

	IMFMediaEvent* Event;
	if (SUCCEEDED(IMFMediaEventGenerator_EndGetEvent(Buddy->Generator, AsyncResult, &Event)))
	{
		MediaEventType Type;
		HR(IMFMediaEvent_GetType(Event, &Type));
		IMFMediaEvent_Release(Event);

		PostMessageW(Buddy->DialogWindow, BUDDY_WM_MEDIA_EVENT, (WPARAM)Type, 0);
	}

	return S_OK;
}

//

static bool Buddy_CreateEncoder(ScreenBuddy* Buddy, int EncodeWidth, int EncodeHeight)
{
	MFT_REGISTER_TYPE_INFO Input = { .guidMajorType = MFMediaType_Video, .guidSubtype = MFVideoFormat_NV12 };
	MFT_REGISTER_TYPE_INFO Output = { .guidMajorType = MFMediaType_Video, .guidSubtype = MFVideoFormat_H264 };

	DXGI_ADAPTER_DESC AdapterDesc;
	{
		IDXGIDevice* DxgiDevice;
		HR(ID3D11Device_QueryInterface(Buddy->Device, &IID_IDXGIDevice, (void**)&DxgiDevice));

		IDXGIAdapter* DxgiAdapter;
		HR(IDXGIDevice_GetAdapter(DxgiDevice, &DxgiAdapter));

		IDXGIAdapter_GetDesc(DxgiAdapter, &AdapterDesc);

		IDXGIAdapter_Release(DxgiAdapter);
		IDXGIDevice_Release(DxgiDevice);
	}

	IMFAttributes* EnumAttributes;
	HR(MFCreateAttributes(&EnumAttributes, 1));
	HR(IMFAttributes_SetBlob(EnumAttributes, &MFT_ENUM_ADAPTER_LUID, (UINT8*)&AdapterDesc.AdapterLuid, sizeof(AdapterDesc.AdapterLuid)));

	IMFActivate** Activate;
	UINT32 ActivateCount;
	HR(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &Input, &Output, EnumAttributes, &Activate, &ActivateCount));
	IMFAttributes_Release(EnumAttributes);

	if (ActivateCount == 0)
	{
		MessageBoxW(Buddy->DialogWindow, L"Cannot create GPU encoder!", L"Error", MB_ICONERROR);
		return false;
	}

	//wchar_t Name[256];
	//HR(IMFActivate_GetString(Activate[0], &MFT_FRIENDLY_NAME_Attribute, Name, ARRAYSIZE(Name), NULL));
	//OutputDebugStringW(Name);

	IMFTransform* Encoder;
	HR(IMFActivate_ActivateObject(Activate[0], &IID_IMFTransform, (void**)&Encoder));
	for (UINT32 Index = 0; Index < ActivateCount; Index++)
	{
		IMFActivate_Release(Activate[Index]);
	}
	CoTaskMemFree(Activate);

	IMFTransform* Converter;
	HR(CoCreateInstance(&CLSID_VideoProcessorMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void**)&Converter));

	{
		IMFAttributes* Attributes;
		HR(IMFTransform_GetAttributes(Converter, &Attributes));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_XVP_PLAYBACK_MODE, TRUE));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_XVP_CALLER_ALLOCATES_OUTPUT, TRUE));
		IMFAttributes_Release(Attributes);
	}

	// unlock async encoder
	{
		IMFAttributes* Attributes;
		HR(IMFTransform_GetAttributes(Encoder, &Attributes));

		UINT32 IsAsync;
		HR(IMFAttributes_GetUINT32(Attributes, &MF_TRANSFORM_ASYNC, &IsAsync));
		Assert(IsAsync);

		HR(IMFAttributes_SetUINT32(Attributes, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
		IMFAttributes_Release(Attributes);
	}

	IMFDXGIDeviceManager* Manager;
	{
		UINT Token;
		HR(MFCreateDXGIDeviceManager(&Token, &Manager));
		HR(IMFDXGIDeviceManager_ResetDevice(Manager, (IUnknown*)Buddy->Device, Token));
		HR(IMFTransform_ProcessMessage(Encoder, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)Manager));
		HR(IMFTransform_ProcessMessage(Converter, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)Manager));
	}

	// enable low latency for encoder, no B-frames, max GOP size
	{
		ICodecAPI* Codec;
		HR(IMFTransform_QueryInterface(Encoder, &IID_ICodecAPI, (void**)&Codec));

		VARIANT LowLatency = { .vt = VT_BOOL, .boolVal = VARIANT_TRUE };
		HR(ICodecAPI_SetValue(Codec, &CODECAPI_AVLowLatencyMode, &LowLatency));

		VARIANT NoBFrames = { .vt = VT_UI4, .ulVal = 0 };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVDefaultBPictureCount, &NoBFrames);

		VARIANT GopMin, GopMax, GopDelta;
		if (SUCCEEDED(ICodecAPI_GetParameterRange(Codec, &CODECAPI_AVEncMPVGOPSize, &GopMin, &GopMax, &GopDelta)))
		{
			HR(ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVGOPSize, &GopMax));
			VariantClear(&GopMin);
			VariantClear(&GopMax);
			VariantClear(&GopDelta);
		}
		else
		{
			VARIANT GopSize = { .vt = VT_UI4, .ulVal = BUDDY_ENCODE_FRAMERATE * 3600 };
			ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVGOPSize, &GopSize);
		}

		ICodecAPI_Release(Codec);
	}

	IMFMediaType* InputType;
	HR(MFCreateMediaType(&InputType));
	HR(IMFMediaType_SetGUID(InputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
	HR(IMFMediaType_SetGUID(InputType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32));
	HR(IMFMediaType_SetUINT32(InputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	HR(IMFMediaType_SetUINT64(InputType, &MF_MT_FRAME_RATE, MF64(BUDDY_ENCODE_FRAMERATE, 1)));
	HR(IMFMediaType_SetUINT64(InputType, &MF_MT_FRAME_SIZE, MF64(EncodeWidth, EncodeHeight)));

	IMFMediaType* ConvertedType;
	HR(MFCreateMediaType(&ConvertedType));
	HR(IMFMediaType_SetGUID(ConvertedType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
	HR(IMFMediaType_SetGUID(ConvertedType, &MF_MT_SUBTYPE, &MFVideoFormat_NV12));
	HR(IMFMediaType_SetUINT32(ConvertedType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	HR(IMFMediaType_SetUINT64(ConvertedType, &MF_MT_FRAME_RATE, MF64(BUDDY_ENCODE_FRAMERATE, 1)));
	HR(IMFMediaType_SetUINT64(ConvertedType, &MF_MT_FRAME_SIZE, MF64(EncodeWidth, EncodeHeight)));

	IMFMediaType* OutputType;
	HR(MFCreateMediaType(&OutputType));
	HR(IMFMediaType_SetGUID(OutputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
	HR(IMFMediaType_SetGUID(OutputType, &MF_MT_SUBTYPE, &MFVideoFormat_H264));
	HR(IMFMediaType_SetUINT32(OutputType, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
	HR(IMFMediaType_SetUINT32(OutputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	HR(IMFMediaType_SetUINT32(OutputType, &MF_MT_AVG_BITRATE, BUDDY_ENCODE_BITRATE));
	HR(IMFMediaType_SetUINT64(OutputType, &MF_MT_FRAME_RATE, MF64(BUDDY_ENCODE_FRAMERATE, 1)));
	HR(IMFMediaType_SetUINT64(OutputType, &MF_MT_FRAME_SIZE, MF64(EncodeWidth, EncodeHeight)));
	HR(IMFMediaType_SetUINT64(OutputType, &MF_MT_PIXEL_ASPECT_RATIO, MF64(1, 1)));

	HR(IMFTransform_SetOutputType(Converter, 0, ConvertedType, 0));
	HR(IMFTransform_SetInputType(Converter, 0, InputType, 0));

	HR(IMFTransform_SetOutputType(Encoder, 0, OutputType, 0));
	HR(IMFTransform_SetInputType(Encoder, 0, ConvertedType, 0));

	MFT_OUTPUT_STREAM_INFO OutputInfo;

	HR(IMFTransform_GetOutputStreamInfo(Converter, 0, &OutputInfo));
	Assert((OutputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0);

	HR(IMFTransform_GetOutputStreamInfo(Encoder, 0, &OutputInfo));
	Assert(OutputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);

	HR(IMFTransform_ProcessMessage(Converter, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
	HR(IMFTransform_ProcessMessage(Encoder, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

	static IMFAsyncCallbackVtbl Buddy__IMFAsyncCallbackVtbl =
	{
		.QueryInterface = &Buddy__QueryInterface,
		.AddRef         = &Buddy__AddRef,
		.Release        = &Buddy__Release,
		.GetParameters  = &Buddy__GetParameters,
		.Invoke         = &Buddy__Invoke,
	};
	Buddy->EventCallback.lpVtbl = &Buddy__IMFAsyncCallbackVtbl;

	IMFVideoSampleAllocatorEx* SampleAllocator;
	HR(MFCreateVideoSampleAllocatorEx(&IID_IMFVideoSampleAllocatorEx, (void**)&SampleAllocator));
	HR(IMFVideoSampleAllocatorEx_SetDirectXManager(SampleAllocator, (IUnknown*)Manager));
	{
		IMFAttributes* Attributes;
		HR(MFCreateAttributes(&Attributes, 2));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT));
		HR(IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(SampleAllocator, 0, BUDDY_ENCODE_QUEUE_SIZE, Attributes, ConvertedType));
		IMFAttributes_Release(Attributes);
	}

	IMFMediaType_Release(InputType);
	IMFMediaType_Release(ConvertedType);
	IMFMediaType_Release(OutputType);
	IMFDXGIDeviceManager_Release(Manager);

	Buddy->EncodeWaitingForInput = false;
	Buddy->EncodeQueueRead = 0;
	Buddy->EncodeQueueWrite = 0;

	Buddy->EncodeNextTime = 0;
	Buddy->EncodeFirstTime = 0;

	Buddy->EncodeSampleAllocator = SampleAllocator;
	Buddy->Codec = Encoder;
	Buddy->Converter = Converter;
	HR(IMFTransform_QueryInterface(Encoder, &IID_IMFMediaEventGenerator, (void**)&Buddy->Generator));

	return true;
}

static bool Buddy_ResetDecoder(IMFTransform* Decoder, IMFTransform* Converter)
{
	DWORD DecodedIndex = 0;
	IMFMediaType* DecodedType = NULL;
	while (SUCCEEDED(IMFTransform_GetOutputAvailableType(Decoder, 0, DecodedIndex, &DecodedType)))
	{
		GUID Format;
		if (SUCCEEDED(IMFMediaType_GetGUID(DecodedType, &MF_MT_SUBTYPE, &Format)))
		{
			if (IsEqualGUID(&Format, &MFVideoFormat_NV12))
			{
				break;
			}
			IMFMediaType_Release(DecodedType);
			DecodedType = NULL;
		}
		DecodedIndex++;
	}
	Assert(DecodedType);

	HR(IMFTransform_SetOutputType(Decoder, 0, DecodedType, 0));
	HR(IMFTransform_SetInputType(Converter, 0, DecodedType, 0));

	UINT64 FrameRate;
	HR(IMFMediaType_GetUINT64(DecodedType, &MF_MT_FRAME_RATE, &FrameRate));

	UINT64 FrameSize;
	HR(IMFMediaType_GetUINT64(DecodedType, &MF_MT_FRAME_SIZE, &FrameSize));

	IMFMediaType* OutputType;
	HR(MFCreateMediaType(&OutputType));
	HR(IMFMediaType_SetGUID(OutputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
	HR(IMFMediaType_SetGUID(OutputType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32));
	HR(IMFMediaType_SetUINT32(OutputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	HR(IMFMediaType_SetUINT64(OutputType, &MF_MT_FRAME_RATE, FrameRate));
	HR(IMFMediaType_SetUINT64(OutputType, &MF_MT_FRAME_SIZE, FrameSize));
	HR(IMFTransform_SetOutputType(Converter, 0, OutputType, 0));

	IMFMediaType_Release(DecodedType);
	IMFMediaType_Release(OutputType);

	return true;
}

static bool Buddy_CreateDecoder(ScreenBuddy* Buddy)
{
	MFT_REGISTER_TYPE_INFO Input = { .guidMajorType = MFMediaType_Video, .guidSubtype = MFVideoFormat_H264 };
	MFT_REGISTER_TYPE_INFO Output = { .guidMajorType = MFMediaType_Video, .guidSubtype = MFVideoFormat_NV12 };

	IMFActivate** Activate;
	UINT32 ActivateCount;
	HR(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &Input, &Output, &Activate, &ActivateCount));

	if (ActivateCount == 0)
	{
		MessageBoxW(Buddy->DialogWindow, L"Cannot create GPU decoder!", L"Error", MB_ICONERROR);
		return false;
	}

	IMFTransform* Decoder;
	HR(IMFActivate_ActivateObject(Activate[0], &IID_IMFTransform, (void**)&Decoder));
	for (UINT32 Index = 0; Index < ActivateCount; Index++)
	{
		IMFActivate_Release(Activate[Index]);
	}
	CoTaskMemFree(Activate);

	IMFTransform* Converter;
	HR(CoCreateInstance(&CLSID_VideoProcessorMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void**)&Converter));

	{
		IMFAttributes* Attributes;
		HR(IMFTransform_GetAttributes(Converter, &Attributes));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_XVP_PLAYBACK_MODE, TRUE));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_XVP_CALLER_ALLOCATES_OUTPUT, TRUE));
		IMFAttributes_Release(Attributes);
	}
	
	{
		UINT Token;
		IMFDXGIDeviceManager* Manager;
		HR(MFCreateDXGIDeviceManager(&Token, &Manager));
		HR(IMFDXGIDeviceManager_ResetDevice(Manager, (IUnknown*)Buddy->Device, Token));
		HR(IMFTransform_ProcessMessage(Decoder, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)Manager));
		HR(IMFTransform_ProcessMessage(Converter, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)Manager));
		IMFDXGIDeviceManager_Release(Manager);
	}

	// enable low latency for decoder
	{
		ICodecAPI* Codec;
		HR(IMFTransform_QueryInterface(Decoder, &IID_ICodecAPI, (void**)&Codec));

		VARIANT LowLatency = { .vt = VT_UI4, .boolVal = VARIANT_TRUE };
		HR(ICodecAPI_SetValue(Codec, &CODECAPI_AVLowLatencyMode, &LowLatency));

		ICodecAPI_Release(Codec);
	}

	IMFMediaType* InputType;
	HR(MFCreateMediaType(&InputType));
	HR(IMFMediaType_SetGUID(InputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
	HR(IMFMediaType_SetGUID(InputType, &MF_MT_SUBTYPE, &MFVideoFormat_H264));
	HR(IMFMediaType_SetUINT32(InputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	HR(IMFMediaType_SetUINT64(InputType, &MF_MT_FRAME_RATE, MF64(BUDDY_ENCODE_FRAMERATE, 1)));
	HR(IMFMediaType_SetUINT64(InputType, &MF_MT_FRAME_SIZE, MF64(4, 4)));
	HR(IMFMediaType_SetUINT64(InputType, &MF_MT_PIXEL_ASPECT_RATIO, MF64(1, 1)));

	HR(IMFTransform_SetInputType(Decoder, 0, InputType, 0));
	IMFMediaType_Release(InputType);

	if (!Buddy_ResetDecoder(Decoder, Converter))
	{
		Assert(0);
	}

	MFT_OUTPUT_STREAM_INFO OutputInfo;

	HR(IMFTransform_GetOutputStreamInfo(Decoder, 0, &OutputInfo));
	Assert(OutputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);

	HR(IMFTransform_GetOutputStreamInfo(Converter, 0, &OutputInfo));
	Assert((OutputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0);

	HR(IMFTransform_ProcessMessage(Decoder, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
	HR(IMFTransform_ProcessMessage(Converter, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

	Buddy->DecodeInputExpected = 0;
	Buddy->DecodeInputBuffer = NULL;
	Buddy->DecodeOutputSample = NULL;
	Buddy->Codec = Decoder;
	Buddy->Converter = Converter;

	return true;
}

static void Buddy_NextMediaEvent(ScreenBuddy* Buddy)
{
	IMFMediaEventGenerator_BeginGetEvent(Buddy->Generator, &Buddy->EventCallback, NULL);
}

static void Buddy_InputToEncoder(ScreenBuddy* Buddy)
{
	if (Buddy->EncodeQueueWrite - Buddy->EncodeQueueRead == 0)
	{
		Buddy->EncodeWaitingForInput = true;
		return;
	}

	IMFSample* Sample = Buddy->EncodeQueue[Buddy->EncodeQueueRead % BUDDY_ENCODE_QUEUE_SIZE];
	Buddy->EncodeQueueRead += 1;

	HR(IMFTransform_ProcessInput(Buddy->Codec, 0, Sample, 0));
	IMFSample_Release(Sample);
}

static void Buddy_Disconnect(ScreenBuddy* Buddy, const wchar_t* Message);

static void Buddy_OutputFromEncoder(ScreenBuddy* Buddy)
{
	DWORD Status;
	MFT_OUTPUT_DATA_BUFFER Output = { .pSample = NULL };

	for (;;)
	{
		HRESULT hr = IMFTransform_ProcessOutput(Buddy->Codec, 0, 1, &Output, &Status);
		if (SUCCEEDED(hr))
		{
			break;
		}
		else if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			DWORD OutputIndex = 0;
			IMFMediaType* OutputType = NULL;
			while (SUCCEEDED(IMFTransform_GetOutputAvailableType(Buddy->Codec, 0, OutputIndex, &OutputType)))
			{
				GUID Format;
				if (SUCCEEDED(IMFMediaType_GetGUID(OutputType, &MF_MT_SUBTYPE, &Format)))
				{
					if (IsEqualGUID(&Format, &MFVideoFormat_H264))
					{
						break;
					}
					IMFMediaType_Release(OutputType);
					OutputType = NULL;
				}
				OutputIndex++;
			}
			Assert(OutputType);

			HR(IMFTransform_SetOutputType(Buddy->Codec, 0, OutputType, 0));
			return;
		}
		else
		{
			HR(hr);
		}
	}

	IMFSample* OutputSample = Output.pSample;

	IMFMediaBuffer* OutputBuffer;
	HR(IMFSample_ConvertToContiguousBuffer(OutputSample, &OutputBuffer));

	BYTE* OutputData;
	DWORD OutputSize;
	HR(IMFMediaBuffer_Lock(OutputBuffer, &OutputData, NULL, &OutputSize));

	uint8_t SendBuffer[65000];

	uint8_t Extra[1 + sizeof(OutputSize)];
	uint32_t ExtraSize = sizeof(Extra);

	Extra[0] = BUDDY_PACKET_VIDEO;
	CopyMemory(Extra + 1, &OutputSize, sizeof(OutputSize));

	while (OutputSize != 0)
	{
		if (ExtraSize)
		{
			CopyMemory(SendBuffer, Extra, ExtraSize);
		}

		uint32_t SendSize = min(OutputSize, sizeof(SendBuffer) - ExtraSize);
		CopyMemory(SendBuffer + ExtraSize, OutputData, SendSize);

		if (!DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, SendBuffer, SendSize + ExtraSize))
		{
			Buddy_Disconnect(Buddy, L"DerpNet disconnect while sending data!");
			break;
		}

		OutputData += SendSize;
		OutputSize -= SendSize;

		ExtraSize = 1;
	}

	HR(IMFMediaBuffer_Unlock(OutputBuffer));

	IMFMediaBuffer_Release(OutputBuffer);
	IMFSample_Release(OutputSample);
}

static void Buddy_Decode(ScreenBuddy* Buddy, IMFMediaBuffer* InputBuffer)
{
	IMFSample* InputSample;
	HR(MFCreateSample(&InputSample));
	HR(IMFSample_AddBuffer(InputSample, InputBuffer));

	HR(IMFTransform_ProcessInput(Buddy->Codec, 0, InputSample, 0));
	IMFSample_Release(InputSample);

	bool NewFrameDecoded = false;
	for (;;)
	{
		DWORD Status;
		MFT_OUTPUT_DATA_BUFFER Output = { .pSample = NULL };

		HRESULT hr = IMFTransform_ProcessOutput(Buddy->Codec, 0, 1, &Output, &Status);
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			Buddy_ResetDecoder(Buddy->Codec, Buddy->Converter);

			if (Buddy->DecodeOutputSample)
			{
				IMFSample_Release(Buddy->DecodeOutputSample);
				Buddy->DecodeOutputSample = NULL;
			}
			continue;
		}
		else if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			break;
		}
		HR(hr);

		IMFSample* DecodedSample = Output.pSample;

		if (Buddy->DecodeOutputSample == NULL)
		{
			IMFMediaBuffer* DecodedBuffer;
			HR(IMFSample_GetBufferByIndex(DecodedSample, 0, &DecodedBuffer));

			IMFDXGIBuffer* DxgiBuffer;
			HR(IMFMediaBuffer_QueryInterface(DecodedBuffer, &IID_IMFDXGIBuffer, (void**)&DxgiBuffer));

			ID3D11Texture2D* DecodedTexture;
			HR(IMFDXGIBuffer_GetResource(DxgiBuffer, &IID_ID3D11Texture2D, (void**)&DecodedTexture));

			D3D11_TEXTURE2D_DESC DecodedDesc;
			ID3D11Texture2D_GetDesc(DecodedTexture, &DecodedDesc);

			int DecodedWidth = DecodedDesc.Width;
			int DecodedHeight = DecodedDesc.Height;

			ID3D11Texture2D_Release(DecodedTexture);
			IMFDXGIBuffer_Release(DxgiBuffer);
			IMFMediaBuffer_Release(DecodedBuffer);

			D3D11_TEXTURE2D_DESC TextureDesc =
			{
				.Width = DecodedWidth,
				.Height = DecodedHeight,
				.MipLevels = 0,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
				.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS,
			};

			ID3D11Texture2D* Texture;
			ID3D11Device_CreateTexture2D(Buddy->Device, &TextureDesc, NULL, &Texture);

			if (Buddy->InputView)
			{
				ID3D11ShaderResourceView_Release(Buddy->InputView);
			}
			ID3D11Device_CreateShaderResourceView(Buddy->Device, (ID3D11Resource*)Texture, NULL, &Buddy->InputView);

			Buddy->InputMipsGenerated = false;
			Buddy->InputWidth = DecodedWidth;
			Buddy->InputHeight = DecodedHeight;

			//

			IMFMediaBuffer* OutputBuffer;
			HR(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown*)Texture, 0, FALSE, &OutputBuffer));

			DWORD OutputLength;
			HR(IMFMediaBuffer_GetMaxLength(OutputBuffer, &OutputLength));
			HR(IMFMediaBuffer_SetCurrentLength(OutputBuffer, OutputLength));

			HR(MFCreateSample(&Buddy->DecodeOutputSample));
			HR(IMFSample_AddBuffer(Buddy->DecodeOutputSample, OutputBuffer));

			HR(IMFSample_SetSampleDuration(Buddy->DecodeOutputSample, 10 * 1000 * 1000 / BUDDY_ENCODE_FRAMERATE));
			HR(IMFSample_SetSampleTime(Buddy->DecodeOutputSample, 0));

			IMFMediaBuffer_Release(OutputBuffer);
			ID3D11Texture2D_Release(Texture);
		}

		HR(IMFTransform_ProcessInput(Buddy->Converter, 0, DecodedSample, 0));
		IMFSample_Release(DecodedSample);

		MFT_OUTPUT_DATA_BUFFER ConverterOutput = { .pSample = Buddy->DecodeOutputSample };
		HR(IMFTransform_ProcessOutput(Buddy->Converter, 0, 1, &ConverterOutput, &Status));

		NewFrameDecoded = true;
		Buddy->InputMipsGenerated = false;
	}

	if (NewFrameDecoded)
	{
		InvalidateRect(Buddy->MainWindow, NULL, FALSE);
	}
}

static void Buddy_OnFrameCapture(ScreenCapture* Capture, bool Closed) 
{
	ScreenBuddy* Buddy = CONTAINING_RECORD(Capture, ScreenBuddy, Capture);

	if (Buddy->State != BUDDY_STATE_SHARING)
	{
		return;
	}

	ScreenCaptureFrame Frame;
	if (ScreenCapture_GetFrame(&Buddy->Capture, &Frame))
	{
		if (Frame.Time > Buddy->EncodeNextTime)
		{
			IMFSample* ConvertedSample;
			if (SUCCEEDED(IMFVideoSampleAllocatorEx_AllocateSample(Buddy->EncodeSampleAllocator, &ConvertedSample)))
			{
				if (Buddy->EncodeFirstTime == 0)
				{
					Buddy->EncodeFirstTime = Frame.Time;
				}
				Buddy->EncodeNextTime = Frame.Time + Buddy->Freq / BUDDY_ENCODE_FRAMERATE;

				IMFMediaBuffer* InputBuffer;
				HR(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown*)Frame.Texture, 0, FALSE, &InputBuffer));

				DWORD InputBufferLength;
				HR(IMFMediaBuffer_GetMaxLength(InputBuffer, &InputBufferLength));
				HR(IMFMediaBuffer_SetCurrentLength(InputBuffer, InputBufferLength));

				IMFSample* InputSample;
				HR(MFCreateSample(&InputSample));
				HR(IMFSample_AddBuffer(InputSample, InputBuffer));
				IMFMediaBuffer_Release(InputBuffer);

				HR(IMFSample_SetSampleTime(InputSample, MFllMulDiv(Frame.Time - Buddy->EncodeFirstTime, 10 * 1000 * 1000, Buddy->Freq, 0)));
				HR(IMFSample_SetSampleDuration(InputSample, 10 * 1000 * 1000 / BUDDY_ENCODE_FRAMERATE));

				HR(IMFTransform_ProcessInput(Buddy->Converter, 0, InputSample, 0));
				IMFSample_Release(InputSample);

				DWORD Status;
				MFT_OUTPUT_DATA_BUFFER Output = { .pSample = ConvertedSample };
				HR(IMFTransform_ProcessOutput(Buddy->Converter, 0, 1, &Output, &Status));

				if (Buddy->EncodeQueueWrite - Buddy->EncodeQueueRead != BUDDY_ENCODE_QUEUE_SIZE)
				{
					Buddy->EncodeQueue[Buddy->EncodeQueueWrite % BUDDY_ENCODE_QUEUE_SIZE] = ConvertedSample;
					Buddy->EncodeQueueWrite += 1;

					if (Buddy->EncodeWaitingForInput)
					{
						Buddy->EncodeWaitingForInput = false;
						Buddy_InputToEncoder(Buddy);
					}
				}
				else
				{
					IMFSample_Release(ConvertedSample);
				}
			}
		}
		ScreenCapture_ReleaseFrame(&Buddy->Capture, &Frame);
	}
}

//

void Buddy_ShowMessage(ScreenBuddy* Buddy, const wchar_t* Message)
{
	HDC DeviceContext = CreateCompatibleDC(0);
	Assert(DeviceContext);

	int FontHeight = MulDiv(24, GetDeviceCaps(DeviceContext, LOGPIXELSY), 72);;
	HFONT Font = CreateFontW(-FontHeight, 0, 0, 0, FW_BOLD,
		FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
	Assert(Font);

	SelectObject(DeviceContext, Font);

	int MessageLength = lstrlenW(Message);

	SIZE Size;
	GetTextExtentPoint32W(DeviceContext, Message, MessageLength, &Size);
	int Width = Size.cx + 2 * FontHeight;
	int Height = Size.cy + 2 * FontHeight;

	BITMAPINFO Info =
	{
		.bmiHeader =
		{
			.biSize = sizeof(Info.bmiHeader),
			.biWidth = Width,
			.biHeight = -Height,
			.biPlanes = 1,
			.biBitCount = 32,
			.biCompression = BI_RGB,
		},
	};

	void* Bits;
	HBITMAP Bitmap = CreateDIBSection(DeviceContext, &Info, DIB_RGB_COLORS, &Bits, NULL, 0);
	Assert(Bitmap);

	SelectObject(DeviceContext, Bitmap);
	SetTextAlign(DeviceContext, TA_CENTER | TA_BASELINE);
	SetTextColor(DeviceContext, RGB(255, 255, 255));
	SetBkColor(DeviceContext, RGB(0, 0, 0));
	ExtTextOutW(DeviceContext, Width / 2, Height / 2, ETO_OPAQUE, NULL, Message, MessageLength, NULL);

	D3D11_TEXTURE2D_DESC TextureDesc =
	{
		.Width = Width,
		.Height = Height,
		.MipLevels = 0,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
		.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS,
	};

	ID3D11Texture2D* Texture;
	ID3D11Device_CreateTexture2D(Buddy->Device, &TextureDesc, NULL, &Texture);

	D3D11_BOX Box =
	{
		.left = 0,
		.top = 0,
		.front = 0,
		.right = Width,
		.bottom = Height,
		.back = 1,
	};
	ID3D11DeviceContext_UpdateSubresource(Buddy->Context, (ID3D11Resource*)Texture, 0, &Box, Bits, Width * 4, 0);

	if (Buddy->InputView)
	{
		ID3D11ShaderResourceView_Release(Buddy->InputView);
	}
	ID3D11Device_CreateShaderResourceView(Buddy->Device, (ID3D11Resource*)Texture, NULL, &Buddy->InputView);
	ID3D11Texture2D_Release(Texture);

	DeleteObject(Bitmap);
	DeleteObject(Font);
	DeleteDC(DeviceContext);

	Buddy->InputMipsGenerated = false;
	Buddy->InputWidth = Width;
	Buddy->InputHeight = Height;

	InvalidateRect(Buddy->MainWindow, NULL, FALSE);
}

static void Buddy_CreateRendering(ScreenBuddy* Buddy, HWND Window)
{
	Buddy->InputMipsGenerated = false;
	Buddy->InputWidth = 0;
	Buddy->InputHeight = 0;
	Buddy->OutputWidth = 0;
	Buddy->OutputHeight = 0;
	Buddy->InputView = NULL;
	Buddy->OutputView = NULL;
}

static void Buddy_ReleaseRendering(ScreenBuddy* Buddy)
{
	ID3D11DeviceContext_ClearState(Buddy->Context);

	if (Buddy->InputView)
	{
		ID3D11ShaderResourceView_Release(Buddy->InputView);
	}
	if (Buddy->OutputView)
	{
		ID3D11RenderTargetView_Release(Buddy->OutputView);
	}

	ID3D11PixelShader_Release(Buddy->PixelShader);
	ID3D11PixelShader_Release(Buddy->VertexShader);
	ID3D11Buffer_Release(Buddy->ConstantBuffer);
	IDXGISwapChain1_Release(Buddy->SwapChain);
}

static void Buddy_RenderWindow(ScreenBuddy* Buddy)
{
	RECT ClientRect;
	GetClientRect(Buddy->MainWindow, &ClientRect);

	int WindowWidth = ClientRect.right - ClientRect.left;
	int WindowHeight = ClientRect.bottom - ClientRect.top;

	if (WindowWidth == 0 || WindowHeight == 0)
	{
		return;
	}

	if (WindowWidth != Buddy->OutputWidth || WindowHeight != Buddy->OutputHeight)
	{
		if (Buddy->OutputView)
		{
			ID3D11DeviceContext_ClearState(Buddy->Context);
			ID3D11RenderTargetView_Release(Buddy->OutputView);
			Buddy->OutputView = NULL;
		}

		HR(IDXGISwapChain1_ResizeBuffers(Buddy->SwapChain, 0, WindowWidth, WindowHeight, DXGI_FORMAT_UNKNOWN, 0));
		Buddy->OutputWidth = WindowWidth;
		Buddy->OutputHeight = WindowHeight;
	}

	if (Buddy->OutputView == NULL)
	{
		ID3D11Texture2D* OutputTexture;
		HR(IDXGISwapChain1_GetBuffer(Buddy->SwapChain, 0, &IID_ID3D11Texture2D, (void**)&OutputTexture));
		ID3D11Device_CreateRenderTargetView(Buddy->Device, (ID3D11Resource*)OutputTexture, NULL, &Buddy->OutputView);
		ID3D11Texture2D_Release(OutputTexture);
	}

	Assert(Buddy->InputView != NULL);
	int InputWidth = Buddy->InputWidth;
	int InputHeight = Buddy->InputHeight;
	int OutputWidth = Buddy->OutputWidth;
	int OutputHeight = Buddy->OutputHeight;

	if (OutputWidth * InputHeight < OutputHeight * InputWidth)
	{
		if (OutputWidth < InputWidth)
		{
			OutputHeight = InputHeight * OutputWidth / InputWidth;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else
	{
		if (OutputHeight < InputHeight)
		{
			OutputWidth = InputWidth * OutputHeight / InputHeight;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}

	ID3D11DeviceContext* Context = Buddy->Context;

	bool IsPartiallyCovered = OutputWidth != WindowWidth || OutputHeight != WindowHeight;
	if (IsPartiallyCovered)
	{
		float BackgroundColor[4] = { 0, 0, 0, 0 };
		ID3D11DeviceContext_ClearRenderTargetView(Context, Buddy->OutputView, BackgroundColor);
	}

	bool IsInputLarger = InputWidth > OutputWidth || InputHeight > OutputHeight;
	if (IsInputLarger)
	{
		if (!Buddy->InputMipsGenerated)
		{
			ID3D11DeviceContext_GenerateMips(Context, Buddy->InputView);
			Buddy->InputMipsGenerated = true;
		}
	}

	D3D11_MAPPED_SUBRESOURCE Mapped;
	HR(ID3D11DeviceContext_Map(Context, (ID3D11Resource*)Buddy->ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped));
	{
		int W = OutputWidth;
		int H = OutputHeight;
		int X = (WindowWidth - OutputWidth) / 2;
		int Y = (WindowHeight - OutputHeight) / 2;

		float* Data = Mapped.pData;
		Data[0] = (float)W / WindowWidth;
		Data[1] = (float)H / WindowHeight;
		Data[2] = (float)X / WindowWidth;
		Data[3] = (float)Y / WindowHeight;
	}
	ID3D11DeviceContext_Unmap(Context, (ID3D11Resource*)Buddy->ConstantBuffer, 0);

	D3D11_VIEWPORT Viewport =
	{
		.Width = (float)WindowWidth,
		.Height = (float)WindowHeight,
	};

	ID3D11DeviceContext_ClearState(Context);
	ID3D11DeviceContext_IASetPrimitiveTopology(Context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ID3D11DeviceContext_VSSetConstantBuffers(Context, 0, 1, &Buddy->ConstantBuffer);
	ID3D11DeviceContext_VSSetShader(Context, Buddy->VertexShader, NULL, 0);
	ID3D11DeviceContext_RSSetViewports(Context, 1, &Viewport);
	ID3D11DeviceContext_PSSetShaderResources(Context, 0, 1, &Buddy->InputView);
	ID3D11DeviceContext_PSSetShader(Context, Buddy->PixelShader, NULL, 0);
	ID3D11DeviceContext_OMSetRenderTargets(Context, 1, &Buddy->OutputView, NULL);
	ID3D11DeviceContext_Draw(Context, 4, 0);

	HR(IDXGISwapChain1_Present(Buddy->SwapChain, 0, 0));
}

static bool Buddy_GetMousePosition(ScreenBuddy* Buddy, Buddy_MousePacket* Packet, int X, int Y)
{
	int InputWidth = Buddy->InputWidth;
	int InputHeight = Buddy->InputHeight;
	int OutputWidth = Buddy->OutputWidth;
	int OutputHeight = Buddy->OutputHeight;

	if (OutputWidth == 0 || OutputHeight == 0)
	{
		return false;
	}

	int WindowWidth = OutputWidth;
	int WindowHeight = OutputHeight;

	if (OutputWidth * InputHeight < OutputHeight * InputWidth)
	{
		if (OutputWidth < InputWidth)
		{
			OutputHeight = InputHeight * OutputWidth / InputWidth;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else
	{
		if (OutputHeight < InputHeight)
		{
			OutputWidth = InputWidth * OutputHeight / InputHeight;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}

	int OffsetX = (WindowWidth - OutputWidth) / 2;
	int OffsetY = (WindowHeight - OutputHeight) / 2;

	X = (X - OffsetX) * InputWidth / OutputWidth;
	Y = (Y - OffsetY) * InputHeight / OutputHeight;

	Packet->X = X < INT16_MIN ? INT16_MIN : X > INT16_MAX ? INT16_MAX : X;
	Packet->Y = Y < INT16_MIN ? INT16_MIN : Y > INT16_MAX ? INT16_MAX : Y;

	return true;
}

static void Buddy_UpdateState(ScreenBuddy* Buddy, BuddyState NewState)
{
	bool Disconnected = NewState == BUDDY_STATE_INITIAL || NewState == BUDDY_STATE_DISCONNECTED;
	bool Sharing = NewState == BUDDY_STATE_SHARE_STARTED || NewState == BUDDY_STATE_SHARING;
	bool Connecting = NewState == BUDDY_STATE_CONNECTING || NewState == BUDDY_STATE_CONNECTED;

	Button_SetText(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_SHARE_BUTTON), Disconnected || Connecting ? L"Share" : L"Stop");
	Button_SetText(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_CONNECT_BUTTON), Disconnected || Sharing ? L"Connect" : L"Disconnect");

	EnableWindow(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_SHARE_BUTTON), Disconnected || Sharing);
	EnableWindow(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_CONNECT_BUTTON), Disconnected || Connecting);

	EnableWindow(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_SHARE_NEW), Disconnected);
	EnableWindow(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_CONNECT_PASTE), Disconnected);
	EnableWindow(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_SHARE_KEY), Disconnected);
	EnableWindow(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_CONNECT_KEY), Disconnected);

	Buddy->State = NewState;
}

static void Buddy_StopDecoder(ScreenBuddy* Buddy)
{
	IMFTransform_Release(Buddy->Codec);
	IMFTransform_Release(Buddy->Converter);

	if (Buddy->DecodeInputBuffer)
	{
		IMFMediaBuffer_Release(Buddy->DecodeInputBuffer);
	}
	if (Buddy->DecodeOutputSample)
	{
		IMFSample_Release(Buddy->DecodeOutputSample);
	}
}

static void Buddy_StopSharing(ScreenBuddy* Buddy)
{
	if (Buddy->State == BUDDY_STATE_SHARING)
	{
		ScreenCapture_Stop(&Buddy->Capture);
	}

	IMFShutdown* Shutdown;
	HR(IMFTransform_QueryInterface(Buddy->Codec, &IID_IMFShutdown, (void**)&Shutdown));
	HR(IMFShutdown_Shutdown(Shutdown));
	IMFShutdown_Release(Shutdown);

	IMFTransform_Release(Buddy->Codec);
	IMFTransform_Release(Buddy->Converter);
	IMFVideoSampleAllocatorEx_Release(Buddy->EncodeSampleAllocator);

	ScreenCapture_Release(&Buddy->Capture);
}

static void Buddy_Disconnect(ScreenBuddy* Buddy, const wchar_t* Message)
{
	if (Buddy->State == BUDDY_STATE_CONNECTING || Buddy->State == BUDDY_STATE_CONNECTED)
	{
		if (Buddy->State == BUDDY_STATE_CONNECTING)
		{
			KillTimer(Buddy->MainWindow, BUDDY_DISCONNECT_TIMER);
		}
		Buddy_StopDecoder(Buddy);

		Buddy_ShowMessage(Buddy, Message);
		Buddy_CancelWait(Buddy);
		DerpNet_Close(&Buddy->Net);
	}
	else if (Buddy->State == BUDDY_STATE_SHARE_STARTED || Buddy->State == BUDDY_STATE_SHARING)
	{
		Buddy_StopSharing(Buddy);

		MessageBoxW(Buddy->DialogWindow, Message, BUDDY_TITLE, MB_ICONERROR);
		Buddy_CancelWait(Buddy);
		DerpNet_Close(&Buddy->Net);
	}

	Buddy_UpdateState(Buddy, BUDDY_STATE_DISCONNECTED);
}

static LRESULT CALLBACK Buddy_WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_NCCREATE)
	{
		SetWindowLongPtrW(Window, GWLP_USERDATA, (LONG_PTR)(((CREATESTRUCT*)LParam)->lpCreateParams));
		return DefWindowProcW(Window, Message, WParam, LParam);
	}

	ScreenBuddy* Buddy = (void*)GetWindowLongPtrW(Window, GWLP_USERDATA);
	if (!Buddy)
	{
		return DefWindowProcW(Window, Message, WParam, LParam);
	}

	switch (Message)
	{
	case WM_CREATE:
		Buddy->LastReceived = 0;
		Buddy_CreateRendering(Buddy, Window);
		Buddy_ShowMessage(Buddy, L"Connecting...");
		SetTimer(Window, BUDDY_UPDATE_TITLE_TIMER, 1000, NULL);
		SetWindowTextW(Window, BUDDY_TITLE);
		return 0;

	case WM_DESTROY:
		Buddy_ReleaseRendering(Buddy);
		Buddy_UpdateState(Buddy, BUDDY_STATE_INITIAL);
		ShowWindow(Buddy->DialogWindow, SW_SHOWDEFAULT);
		return 0;

	case WM_TIMER:
		if (WParam == BUDDY_DISCONNECT_TIMER)
		{
			Buddy_Disconnect(Buddy, L"Timeout while connecting to remote computer!");
		}
		else if (WParam == BUDDY_UPDATE_TITLE_TIMER)
		{
			size_t BytesReceived = Buddy->Net.TotalReceived - Buddy->LastReceived;
			Buddy->LastReceived = Buddy->Net.TotalReceived;

			wchar_t Title[256];
			StrFormat(Title, L"%ls - %.f KB/s", BUDDY_TITLE, (double)BytesReceived / 1024.0);
			SetWindowTextW(Window, Title);
		}
		return 0;

	case WM_PAINT:
		Buddy_RenderWindow(Buddy);
		ValidateRect(Window, NULL);
		return 0;

	case WM_CLOSE:
		if (Buddy->State == BUDDY_STATE_CONNECTING)
		{
			Buddy_CancelWait(Buddy);
			DerpNet_Close(&Buddy->Net);
		}
		else if (Buddy->State == BUDDY_STATE_CONNECTED)
		{
			if (MessageBoxW(Window, L"Do you want to disconnect?", BUDDY_TITLE, MB_ICONQUESTION | MB_YESNO) == IDNO)
			{
				return 0;
			}

			uint8_t Data[1] = { BUDDY_PACKET_DISCONNECT };
			DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, Data, sizeof(Data));

			Buddy_CancelWait(Buddy);
			DerpNet_Close(&Buddy->Net);
		}

		Buddy_UpdateState(Buddy, BUDDY_STATE_DISCONNECTED);
		break;

	case WM_MOUSEMOVE:
	{
		if (Buddy->State == BUDDY_STATE_CONNECTED)
		{
			Buddy_MousePacket Packet =
			{
				.Packet = BUDDY_PACKET_MOUSE_MOVE,
			};
			if (Buddy_GetMousePosition(Buddy, &Packet, GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam)))
			{
				if (!DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, &Packet, sizeof(Packet)))
				{
					Buddy_Disconnect(Buddy, L"DerpNet disconnect while sending data!");
				}
			}
		}
		return 0;
	}

	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	{
		SetCapture(Window);

		if (Buddy->State == BUDDY_STATE_CONNECTED)
		{
			Buddy_MousePacket Packet =
			{
				.Packet = BUDDY_PACKET_MOUSE_BUTTON,
				.Button = Message == WM_LBUTTONDOWN ? 0 : Message == WM_RBUTTONDOWN ? 1 : Message == WM_MBUTTONDOWN ? 2 : -1,
				.IsDownOrHorizontalWheel = 1,
			};
			if (Message == WM_XBUTTONDOWN)
			{
				Packet.Button = HIWORD(WParam) == XBUTTON1 ? 3 : 4;
			}
			if (Buddy_GetMousePosition(Buddy, &Packet, GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam)))
			{
				if (!DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, &Packet, sizeof(Packet)))
				{
					Buddy_Disconnect(Buddy, L"DerpNet disconnect while sending data!");
				}
			}
		}
		return 0;
	}

	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP:
	{
		ReleaseCapture();
		if (Buddy->State == BUDDY_STATE_CONNECTED)
		{
			Buddy_MousePacket Packet =
			{
				.Packet = BUDDY_PACKET_MOUSE_BUTTON,
				.Button = Message == WM_LBUTTONUP ? 0 : Message == WM_RBUTTONUP ? 1 : Message == WM_MBUTTONUP ? 2 : -1,
				.IsDownOrHorizontalWheel = 0,
			};
			if (Message == WM_XBUTTONUP)
			{
				Packet.Button = HIWORD(WParam) == XBUTTON1 ? 3 : 4;
			}
			if (Buddy_GetMousePosition(Buddy, &Packet, GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam)))
			{
				if (!DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, &Packet, sizeof(Packet)))
				{
					Buddy_Disconnect(Buddy, L"DerpNet disconnect while sending data!");
				}
			}
		}
		return 0;
	}

	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
	{
		if (Buddy->State == BUDDY_STATE_CONNECTED)
		{
			POINT Point = { GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam) };
			ScreenToClient(Window, &Point);

			Buddy_MousePacket Packet =
			{
				.Packet = BUDDY_PACKET_MOUSE_WHEEL,
				.Button = GET_WHEEL_DELTA_WPARAM(WParam),
				.IsDownOrHorizontalWheel = Message == WM_MOUSEHWHEEL ? 1 : 0,
			};
			Buddy_GetMousePosition(Buddy, &Packet, Point.x, Point.y);

			if (!DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, &Packet, sizeof(Packet)))
			{
				Buddy_Disconnect(Buddy, L"DerpNet disconnect while sending data!");
			}
		}
		return 0;
	}

	}

	return DefWindowProcW(Window, Message, WParam, LParam);
}

//

static bool Buddy_StartSharing(ScreenBuddy* Buddy)
{
	ScreenCapture_Create(&Buddy->Capture, &Buddy_OnFrameCapture, false);

	HMONITOR Monitor = MonitorFromWindow(Buddy->DialogWindow, MONITOR_DEFAULTTOPRIMARY);
	Assert(Monitor);

	if (ScreenCapture_CreateForMonitor(&Buddy->Capture, Buddy->Device, Monitor, NULL))
	{
		int EncodeWidth = Buddy->Capture.Rect.right - Buddy->Capture.Rect.left;
		int EncodeHeight = Buddy->Capture.Rect.bottom - Buddy->Capture.Rect.top;

		if (Buddy_CreateEncoder(Buddy, EncodeWidth, EncodeHeight))
		{
			char DerpHostName[256];
			if (DERPNET_USE_PLAIN_HTTP)
			{
				lstrcpyA(DerpHostName, "localhost");
			}
			else
			{
				WideCharToMultiByte(CP_UTF8, 0, Buddy->DerpRegions[Buddy->DerpRegion], -1, DerpHostName, ARRAYSIZE(DerpHostName), NULL, NULL);
			}
			if (DerpNet_Open(&Buddy->Net, DerpHostName, &Buddy->MyPrivateKey))
			{
				Buddy_StartWait(Buddy);
				return true;
			}
			else
			{
				MessageBoxW(Buddy->DialogWindow, L"Cannot connect to DerpNet server!", L"Error", MB_ICONERROR);
			}
		}
		else
		{
			MessageBoxW(Buddy->DialogWindow, L"Cannot create GPU video encoder!", L"Error", MB_ICONERROR);
		}
	}
	else
	{
		MessageBoxW(Buddy->DialogWindow, L"Cannot capture monitor output!", L"Error", MB_ICONERROR);
	}

	ScreenCapture_Release(&Buddy->Capture);
	return false;

}

static bool Buddy_StartConnection(ScreenBuddy* Buddy)
{
	wchar_t ConnectKey[256];
	int ConnectKeyLength = Edit_GetText(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_CONNECT_KEY), ConnectKey, ARRAYSIZE(ConnectKey));
	if (ConnectKeyLength != 2 + 2 * 32)
	{
		MessageBoxW(Buddy->DialogWindow, L"Incorrect length for connection code!", BUDDY_TITLE, MB_ICONERROR);
		return false;
	}

	uint8_t Region;
	swscanf(ConnectKey, L"%02hhx", &Region);
	for (int i = 0; i < 32; i++)
	{
		swscanf(ConnectKey + 2 + 2 * i, L"%02hhx", &Buddy->RemoteKey.Bytes[i]);
	}

	if (!Buddy_CreateDecoder(Buddy))
	{
		return false;
	}

	DerpKey NewPrivateKey;
	DerpNet_CreateNewKey(&NewPrivateKey);

	char DerpHostName[256];
	if (DERPNET_USE_PLAIN_HTTP)
	{
		lstrcpyA(DerpHostName, "localhost");
	}
	else
	{
		WideCharToMultiByte(CP_UTF8, 0, Buddy->DerpRegions[Region], -1, DerpHostName, ARRAYSIZE(DerpHostName), NULL, NULL);
	}

	if (!DerpNet_Open(&Buddy->Net, DerpHostName, &NewPrivateKey))
	{
		Buddy_StopDecoder(Buddy);
		MessageBoxW(Buddy->DialogWindow, L"Cannot connect to DerpNet server!", BUDDY_TITLE, MB_ICONERROR);
		return false;
	}

	if (!DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, NULL, 0))
	{
		Buddy_StopDecoder(Buddy);
		return false;
	}
	Buddy_StartWait(Buddy);

	Buddy->MainWindow = CreateWindowExW(
		0, BUDDY_CLASS, BUDDY_TITLE, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, GetModuleHandleW(NULL), Buddy);
	Assert(Buddy->MainWindow);

	D3D11_BUFFER_DESC ConstantBufferDesc =
	{
		.ByteWidth = 4 * sizeof(float),
		.Usage = D3D11_USAGE_DYNAMIC,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
	};
	ID3D11Device_CreateBuffer(Buddy->Device, &ConstantBufferDesc, NULL, &Buddy->ConstantBuffer);

	ID3D11Device_CreateVertexShader(Buddy->Device, ScreenBuddyVS, sizeof(ScreenBuddyVS), NULL, &Buddy->VertexShader);
	ID3D11Device_CreatePixelShader(Buddy->Device, ScreenBuddyPS, sizeof(ScreenBuddyPS), NULL, &Buddy->PixelShader);

	IDXGIDevice* DxgiDevice;
	IDXGIAdapter* DxgiAdapter;
	IDXGIFactory2* DxgiFactory;

	HR(ID3D11Device_QueryInterface(Buddy->Device, &IID_IDXGIDevice, (void**)&DxgiDevice));
	HR(IDXGIDevice_GetAdapter(DxgiDevice, &DxgiAdapter));
	HR(IDXGIAdapter_GetParent(DxgiAdapter, &IID_IDXGIFactory2, (void**)&DxgiFactory));

	DXGI_SWAP_CHAIN_DESC1 Desc =
	{
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = { 1, 0 },
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = 2,
		.Scaling = DXGI_SCALING_NONE,
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
	};
	HR(IDXGIFactory2_CreateSwapChainForHwnd(DxgiFactory, (IUnknown*)Buddy->Device, Buddy->MainWindow, &Desc, NULL, NULL, &Buddy->SwapChain));
	HR(IDXGIFactory2_MakeWindowAssociation(DxgiFactory, Buddy->MainWindow, DXGI_MWA_NO_ALT_ENTER));

	IDXGIFactory2_Release(DxgiFactory);
	IDXGIAdapter_Release(DxgiAdapter);
	IDXGIDevice_Release(DxgiDevice);

	ShowWindow(Buddy->MainWindow, SW_SHOWDEFAULT);
	ShowWindow(Buddy->DialogWindow, SW_HIDE);
	return true;
}

static void Buddy_NetworkEvent(ScreenBuddy* Buddy)
{
	while (Buddy->State != BUDDY_STATE_DISCONNECTED)
	{
		DerpKey RecvKey;
		uint8_t* RecvData;
		uint32_t RecvSize;
		int Recv = DerpNet_Recv(&Buddy->Net, &RecvKey, &RecvData, &RecvSize, false);
		if (Recv < 0)
		{
			Buddy_Disconnect(Buddy, L"DerpNet server disconnected!");
			break;
		}
		else if (Recv == 0)
		{
			break;
		}

		if (Buddy->State == BUDDY_STATE_CONNECTING || Buddy->State == BUDDY_STATE_CONNECTED)
		{
			if (Buddy->State == BUDDY_STATE_CONNECTING)
			{
				KillTimer(Buddy->MainWindow, BUDDY_DISCONNECT_TIMER);
				Buddy_UpdateState(Buddy, BUDDY_STATE_CONNECTED);
			}

			if (RtlEqualMemory(&RecvKey, &Buddy->RemoteKey, sizeof(RecvKey)))
			{
				Assert(RecvSize >= 1);

				uint8_t Packet = RecvData[0];
				RecvData += 1;
				RecvSize -= 1;

				if (Packet == BUDDY_PACKET_VIDEO)
				{
					if (Buddy->DecodeInputExpected == 0)
					{
						Assert(RecvSize >= sizeof(Buddy->DecodeInputExpected));
						CopyMemory(&Buddy->DecodeInputExpected, RecvData, sizeof(Buddy->DecodeInputExpected));

						RecvData += sizeof(Buddy->DecodeInputExpected);
						RecvSize -= sizeof(Buddy->DecodeInputExpected);

						Assert(Buddy->DecodeInputBuffer == NULL);
						HR(MFCreateMemoryBuffer(Buddy->DecodeInputExpected, &Buddy->DecodeInputBuffer));
					}

					Assert(Buddy->DecodeInputBuffer);

					BYTE* BufferData;
					DWORD BufferMaxLength;
					DWORD BufferLength;
					HR(IMFMediaBuffer_Lock(Buddy->DecodeInputBuffer, &BufferData, &BufferMaxLength, &BufferLength));
					{
						Assert(BufferLength + RecvSize <= BufferMaxLength);
						CopyMemory(BufferData + BufferLength, RecvData, RecvSize);
					}
					HR(IMFMediaBuffer_Unlock(Buddy->DecodeInputBuffer));

					BufferLength += RecvSize;
					HR(IMFMediaBuffer_SetCurrentLength(Buddy->DecodeInputBuffer, BufferLength));

					if (BufferLength == Buddy->DecodeInputExpected)
					{
						Buddy_Decode(Buddy, Buddy->DecodeInputBuffer);

						IMFMediaBuffer_Release(Buddy->DecodeInputBuffer);
						Buddy->DecodeInputBuffer = NULL;
						Buddy->DecodeInputExpected = 0;
					}
				}
				else if (Packet == BUDDY_PACKET_DISCONNECT)
				{
					Buddy_Disconnect(Buddy, L"Remote computer stopped sharing!");
					break;
				}
			}
		}
		else if (Buddy->State == BUDDY_STATE_SHARE_STARTED)
		{
			if (RecvSize == 0)
			{
				Buddy->RemoteKey = RecvKey;

				ScreenCapture_Start(&Buddy->Capture, true, true);
				Buddy_NextMediaEvent(Buddy);

				Buddy_UpdateState(Buddy, BUDDY_STATE_SHARING);
			}
			else
			{
				Buddy_Disconnect(Buddy, L"Received unexpected initial packet!");
				break;
			}
		}
		else if (Buddy->State == BUDDY_STATE_SHARING)
		{
			if (RtlEqualMemory(&RecvKey, &Buddy->RemoteKey, sizeof(RecvKey)))
			{
				Assert(RecvSize >= 1);

				uint8_t Packet = RecvData[0];
				RecvData += 1;
				RecvSize -= 1;

				if (Packet == BUDDY_PACKET_DISCONNECT)
				{
					DerpNet_Close(&Buddy->Net);
					Buddy_StopSharing(Buddy);
					Buddy_UpdateState(Buddy, BUDDY_STATE_INITIAL);
					break;
				}
				else if (Packet == BUDDY_PACKET_MOUSE_MOVE || Packet == BUDDY_PACKET_MOUSE_BUTTON || Packet == BUDDY_PACKET_MOUSE_WHEEL)
				{
					Buddy_MousePacket Data;
					if (1 + RecvSize == sizeof(Data))
					{
						CopyMemory(&Data.Packet + 1, RecvData, RecvSize);

						MONITORINFO MonitorInfo =
						{
							.cbSize = sizeof(MonitorInfo),
						};
						BOOL MonitorOk = GetMonitorInfoW(Buddy->Capture.Monitor, &MonitorInfo);
						Assert(MonitorOk);

						MONITORINFO PrimaryMonitorInfo =
						{
							.cbSize = sizeof(PrimaryMonitorInfo),
						};
						BOOL PrimaryOk = GetMonitorInfoW(MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY), &PrimaryMonitorInfo);
						Assert(PrimaryOk);

						const RECT* R = &MonitorInfo.rcMonitor;
						const RECT* Primary = &PrimaryMonitorInfo.rcMonitor;

						INPUT Input =
						{
							.type = INPUT_MOUSE,
							.mi.dx = (Data.X + R->left) * 65535 / (Primary->right - Primary->left),
							.mi.dy = (Data.Y + R->top) * 65535 / (Primary->bottom - Primary->top),
							.mi.dwFlags = MOUSEEVENTF_ABSOLUTE,
						};

						if (Packet == BUDDY_PACKET_MOUSE_MOVE)
						{
							Input.mi.dwFlags |= MOUSEEVENTF_MOVE;
							SendInput(1, &Input, sizeof(Input));
						}
						else if (Packet == BUDDY_PACKET_MOUSE_BUTTON)
						{
							switch (Data.Button)
							{
							case 0: Input.mi.dwFlags |= Data.IsDownOrHorizontalWheel ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
							case 1: Input.mi.dwFlags |= Data.IsDownOrHorizontalWheel ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
							case 2: Input.mi.dwFlags |= Data.IsDownOrHorizontalWheel ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
							case 3: Input.mi.dwFlags |= Data.IsDownOrHorizontalWheel ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; Input.mi.mouseData = XBUTTON1; break;
							case 4: Input.mi.dwFlags |= Data.IsDownOrHorizontalWheel ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; Input.mi.mouseData = XBUTTON2; break;
							}
							SendInput(1, &Input, sizeof(Input));
						}
						else if (Packet == BUDDY_PACKET_MOUSE_WHEEL)
						{
							Input.mi.mouseData = Data.Button;
							Input.mi.dwFlags |= (Data.IsDownOrHorizontalWheel ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL);
							SendInput(1, &Input, sizeof(Input));
						}
					}
				}
			}
		}
	}
}

//

static void Dialog_SetTooltip(HWND Dialog, int Control, const char* Text, HWND Tooltip)
{
	wchar_t TooltipText[128];
	MultiByteToWideChar(CP_UTF8, 0, Text, -1, TooltipText, ARRAYSIZE(TooltipText));

	TOOLINFOW TooltipInfo =
	{
		.cbSize = sizeof(TooltipInfo),
		.uFlags = TTF_IDISHWND | TTF_SUBCLASS,
		.hwnd = Dialog,
		.uId = (UINT_PTR)GetDlgItem(Dialog, Control),
		.lpszText = TooltipText,
	};
	SendMessageW(Tooltip, TTM_ADDTOOL, 0, (LPARAM)&TooltipInfo);
}

static void Dialog_ShowShareKey(HWND Control, uint32_t Region, DerpKey* Key)
{
	wchar_t Text[2 + 32 * 2 + 1];
	_swprintf(Text, L"%02hhx", Region);

	for (size_t i = 0; i < sizeof(Key->Bytes); i++)
	{
		_swprintf(Text + 2 + 2 * i, L"%02hhx", Key->Bytes[i]);
	}

	Edit_SetText(Control, Text);
}

static INT_PTR CALLBACK Buddy_DialogProc(HWND Dialog, UINT Message, WPARAM WParam, LPARAM LParam)
{
	ScreenBuddy* Buddy = (void*)GetWindowLongPtr(Dialog, GWLP_USERDATA);

	switch (Message)
	{
	case WM_INITDIALOG:
		Buddy = (void*)LParam;
		SetWindowLongPtrW(Dialog, GWLP_USERDATA, (LONG_PTR)Buddy);

		SendMessageW(Dialog, WM_SETICON, ICON_BIG, (LPARAM)Buddy->Icon);
		Buddy_DialogProc(Dialog, WM_DPICHANGED, 0, 0);

		HWND TooltipWindow = CreateWindowExW(
			0, TOOLTIPS_CLASSW, NULL,
			WS_POPUP | TTS_ALWAYSTIP,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			Dialog, NULL, NULL, NULL);

		Dialog_SetTooltip(Dialog, BUDDY_ID_SHARE_COPY, "Copy", TooltipWindow);
		Dialog_SetTooltip(Dialog, BUDDY_ID_SHARE_NEW, "Generate New Code", TooltipWindow);
		Dialog_SetTooltip(Dialog, BUDDY_ID_CONNECT_PASTE, "Paste", TooltipWindow);

		HWND ShareKey = GetDlgItem(Dialog, BUDDY_ID_SHARE_KEY);

		if (Buddy->DerpRegion == 0)
		{
			Edit_SetText(ShareKey, L"...initializing...");
			Button_Enable(GetDlgItem(Dialog, BUDDY_ID_SHARE_BUTTON), FALSE);

			SetFocus(GetDlgItem(Dialog, BUDDY_ID_SHARE_COPY));

			Buddy->DerpRegionThread = CreateThread(NULL, 0, &Buddy_GetBestDerpRegionThread, Buddy, 0, NULL);
			Assert(Buddy->DerpRegionThread);
		}
		else
		{
			Dialog_ShowShareKey(ShareKey, Buddy->DerpRegion, &Buddy->MyPublicKey);
		}
		PostMessageW(ShareKey, EM_SETSEL, -1, 0);

		return FALSE;

	case WM_DPICHANGED:
	{
		HFONT Font = (HFONT)SendMessageW(Dialog, WM_GETFONT, 0, FALSE);
		Assert(Font);

		LOGFONTW FontInfo;
		GetObject(Font, sizeof(FontInfo), &FontInfo);
		FontInfo.lfHeight *= 5;

		Font = CreateFontIndirectW(&FontInfo);
		Assert(Font);

		SendDlgItemMessageW(Dialog, BUDDY_ID_SHARE_ICON, WM_SETFONT, (WPARAM)Font, FALSE);
		SendDlgItemMessageW(Dialog, BUDDY_ID_CONNECT_ICON, WM_SETFONT, (WPARAM)Font, FALSE);
		return FALSE;
	}

	case WM_CTLCOLORSTATIC:
		if (GetDlgCtrlID((HWND)LParam) == BUDDY_ID_SHARE_KEY)
		{
			return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
		}
		break;

	case WM_CLOSE:
		if (Buddy->State == BUDDY_STATE_SHARING)
		{
			if (MessageBoxW(Dialog, L"Do you want to stop sharing?", BUDDY_TITLE, MB_ICONQUESTION | MB_YESNO) == IDNO)
			{
				break;
			}

			uint8_t Data[1] = { BUDDY_PACKET_DISCONNECT };
			DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, Data, sizeof(Data));
			DerpNet_Close(&Buddy->Net);
			Buddy_StopSharing(Buddy);
		}

		ExitProcess(0);
		return TRUE;

	case WM_COMMAND:
	{
		int Control = LOWORD(WParam);
		if (Control == BUDDY_ID_SHARE_COPY)
		{
			if (OpenClipboard(NULL))
			{
				EmptyClipboard();

				wchar_t ShareKey[128];
				int ShareKeyLen = Edit_GetText(GetDlgItem(Dialog, BUDDY_ID_SHARE_KEY), ShareKey, ARRAYSIZE(ShareKey));

				HGLOBAL ClipboardData = GlobalAlloc(0, (ShareKeyLen + 1) * sizeof(wchar_t));
				Assert(ClipboardData);

				void* ClipboardText = GlobalLock(ClipboardData);
				Assert(ClipboardText);

				CopyMemory(ClipboardText, ShareKey, (ShareKeyLen + 1) * sizeof(wchar_t));

				GlobalUnlock(ClipboardText);
				SetClipboardData(CF_UNICODETEXT, ClipboardData);

				CloseClipboard();
			}
		}
		else if (Control == BUDDY_ID_SHARE_NEW)
		{
			DerpNet_CreateNewKey(&Buddy->MyPrivateKey);
			DerpNet_GetPublicKey(&Buddy->MyPrivateKey, &Buddy->MyPublicKey);

			DATA_BLOB BlobInput = { sizeof(Buddy->MyPrivateKey), Buddy->MyPrivateKey.Bytes };
			DATA_BLOB BlobOutput;
			BOOL Protected = CryptProtectData(&BlobInput, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &BlobOutput);
			Assert(Protected);

			WritePrivateProfileStructW(BUDDY_CONFIG, L"DerpPrivateKey", BlobOutput.pbData, BlobOutput.cbData, Buddy->ConfigPath);
			LocalFree(BlobOutput.pbData);

			Dialog_ShowShareKey(GetDlgItem(Dialog, BUDDY_ID_SHARE_KEY), Buddy->DerpRegion, &Buddy->MyPublicKey);
		}
		else if (Control == BUDDY_ID_CONNECT_PASTE)
		{
			if (OpenClipboard(NULL))
			{
				HANDLE ClipboardData = GetClipboardData(CF_UNICODETEXT);
				if (ClipboardData)
				{
					wchar_t* ClipboardText = GlobalLock(ClipboardData);
					if (ClipboardText)
					{
						Edit_SetText(GetDlgItem(Dialog, BUDDY_ID_CONNECT_KEY), ClipboardText);
						GlobalUnlock(ClipboardText);
					}
				}
				CloseClipboard();
			}
		}
		else if (Control == BUDDY_ID_SHARE_BUTTON)
		{
			if (Buddy->State == BUDDY_STATE_SHARE_STARTED || Buddy->State == BUDDY_STATE_SHARING)
			{
				bool Stop = true;

				if (Buddy->State == BUDDY_STATE_SHARING)
				{
					Stop = MessageBoxW(Dialog, L"Do you want to stop sharing?", BUDDY_TITLE, MB_ICONQUESTION | MB_YESNO) == IDYES;
					if (Stop)
					{
						uint8_t Data[1] = { BUDDY_PACKET_DISCONNECT };
						DerpNet_Send(&Buddy->Net, &Buddy->RemoteKey, Data, sizeof(Data));
					}
				}

				if (Stop)
				{
					Buddy_CancelWait(Buddy);
					DerpNet_Close(&Buddy->Net);
					Buddy_StopSharing(Buddy);
					Buddy_UpdateState(Buddy, BUDDY_STATE_INITIAL);
				}
			}
			else
			{
				if (Buddy_StartSharing(Buddy))
				{
					Buddy_UpdateState(Buddy, BUDDY_STATE_SHARE_STARTED);
				}
			}
		}
		else if (Control == BUDDY_ID_CONNECT_BUTTON)
		{
			if (Buddy_StartConnection(Buddy))
			{
				Buddy_UpdateState(Buddy, BUDDY_STATE_CONNECTING);
				SetTimer(Buddy->MainWindow, BUDDY_DISCONNECT_TIMER, 8 * 1000, NULL);
			}
		}

		return TRUE;
	}

	case BUDDY_WM_BEST_REGION:
	{
		if (WParam == 0)
		{
			MessageBoxW(Buddy->DialogWindow, L"Cannot determine best DERP region!", L"Error", MB_ICONERROR);
			ExitProcess(0);
		}

		Buddy->DerpRegion = (uint32_t)WParam;
		WaitForSingleObject(Buddy->DerpRegionThread, INFINITE);
		CloseHandle(Buddy->DerpRegionThread);

		wchar_t DerpRegionText[128];
		StrFormat(DerpRegionText, L"%u", Buddy->DerpRegion);
		WritePrivateProfileStringW(BUDDY_CONFIG, L"DerpRegion", DerpRegionText, Buddy->ConfigPath);

		for (int RegionIndex = 0; RegionIndex < BUDDY_MAX_REGION_COUNT; RegionIndex++)
		{
			if (Buddy->DerpRegions[RegionIndex][0])
			{
				wchar_t DerpRegionName[128];
				StrFormat(DerpRegionName, L"DerpRegion%d", RegionIndex);
				WritePrivateProfileStringW(BUDDY_CONFIG, DerpRegionName, Buddy->DerpRegions[RegionIndex], Buddy->ConfigPath);
			}
		}

		Button_Enable(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_SHARE_BUTTON), TRUE);

		SetActiveWindow(Buddy->DialogWindow);
		SendDlgItemMessageW(Buddy->DialogWindow, BUDDY_ID_SHARE_NEW, BM_CLICK, 0, 0);
		SetFocus(GetDlgItem(Buddy->DialogWindow, BUDDY_ID_SHARE_KEY));
		return 0;
	}

	case BUDDY_WM_MEDIA_EVENT:
	{
		switch ((MediaEventType)WParam)
		{
		case METransformNeedInput:
			if (Buddy->State == BUDDY_STATE_SHARING)
			{
				Buddy_InputToEncoder(Buddy);
			}
			break;

		case METransformHaveOutput:
			if (Buddy->State == BUDDY_STATE_SHARING)
			{
				Buddy_OutputFromEncoder(Buddy);
			}
			break;
		}
		if (Buddy->State != BUDDY_STATE_DISCONNECTED)
		{
			Buddy_NextMediaEvent(Buddy);
		}
		return 0;
	}

	case BUDDY_WM_NET_EVENT:
		Buddy_NetworkEvent(Buddy);
		if (Buddy->State != BUDDY_STATE_INITIAL && Buddy->State != BUDDY_STATE_DISCONNECTED)
		{
			Buddy_NextWait(Buddy);
		}
		return 0;

	}

	return FALSE;
}

//

enum
{
	// win32 control styles
	BUDDY_DIALOG_BUTTON		= 0x0080,
	BUDDY_DIALOG_EDIT		= 0x0081,
	BUDDY_DIALOG_LABEL		= 0x0082,

	// extra flags for items
	BUDDY_DIALOG_NEW_LINE	= 1<<0,
	BUDDY_DIALOG_READ_ONLY	= 1<<1,
};

typedef struct
{
	int Left;
	int Top;
	int Width;
	int Height;
}
Buddy_DialogRect;

typedef struct
{
	const char* Text;
	const uint16_t Id;
	const uint16_t Control;
	const uint16_t Width;
	const uint16_t Flags;
}
Buddy_DialogItem;

typedef struct
{
	const char* Caption;
	const char* Icon;
	const uint16_t IconId;
	const Buddy_DialogItem* Items;
}
Buddy_DialogGroup;

typedef struct
{
	const wchar_t* Title;
	const char* Font;
	const WORD FontSize;
	const Buddy_DialogGroup* Groups;
}
Buddy_DialogLayout;

static void* Dialog__Align(uint8_t* Data, size_t Size)
{
	uintptr_t Pointer = (uintptr_t)Data;
	return Data + ((Pointer + Size - 1) & ~(Size - 1)) - Pointer;
}

static void* Dialog__DoItem(void* Ptr, const char* Text, uint16_t Id, uint16_t Control, uint32_t Style, int X, int Y, int W, int H)
{
	uint8_t* Data = Dialog__Align(Ptr, sizeof(uint32_t));

	*(DLGITEMTEMPLATE*)Data = (DLGITEMTEMPLATE)
	{
		.style = Style | WS_CHILD | WS_VISIBLE,
		.x = X,
		.y = Y,
		.cx = W,
		.cy = H,
		.id = Id,
	};
	Data += sizeof(DLGITEMTEMPLATE);

	// window class
	Data = Dialog__Align(Data, sizeof(uint16_t));
	*(uint16_t*)Data = 0xffff;
	Data += sizeof(uint16_t);
	*(uint16_t*)Data = Control;
	Data += sizeof(uint16_t);

	// item text
	Data = Dialog__Align(Data, sizeof(wchar_t));
	DWORD ItemChars = MultiByteToWideChar(CP_UTF8, 0, Text, -1, (wchar_t*)Data, 128);
	Data += ItemChars * sizeof(wchar_t);

	// extras
	Data = Dialog__Align(Data, sizeof(uint16_t));
	*(uint16_t*)Data = 0;
	Data += sizeof(uint16_t);

	return Data;
}

static void Buddy_DoDialogLayout(const Buddy_DialogLayout* Dialog, void* Buffer, void* BufferEnd)
{
	uint8_t* Data = Buffer;

	// header
	DLGTEMPLATE* Template = (void*)Data;
	Data += sizeof(DLGTEMPLATE);

	// menu
	Data = Dialog__Align(Data, sizeof(wchar_t));
	*(wchar_t*)Data = 0;
	Data += sizeof(wchar_t);

	// window class
	Data = Dialog__Align(Data, sizeof(wchar_t));
	*(wchar_t*)Data = 0;
	Data += sizeof(wchar_t);

	// title
	Data = Dialog__Align(Data, sizeof(wchar_t));
	lstrcpynW((wchar_t*)Data, Dialog->Title, 128);
	Data += (lstrlenW((wchar_t*)Data) + 1) * sizeof(wchar_t);

	// font size
	Data = Dialog__Align(Data, sizeof(uint16_t));
	*(uint16_t*)Data = Dialog->FontSize;
	Data += sizeof(uint16_t);

	// font name
	Data = Dialog__Align(Data, sizeof(wchar_t));
	DWORD FontChars = MultiByteToWideChar(CP_UTF8, 0, Dialog->Font, -1, (wchar_t*)Data, 128);
	Data += FontChars * sizeof(wchar_t);

	int ItemCount = 0;

	int GroupX = BUDDY_DIALOG_PADDING;
	int GroupY = BUDDY_DIALOG_PADDING;

	for (const Buddy_DialogGroup* Group = Dialog->Groups; Group->Caption; Group++)
	{
		int LineCount = 0;
		for (const Buddy_DialogItem* Item = Group->Items; Item->Text; Item++)
		{
			if (Item->Flags & BUDDY_DIALOG_NEW_LINE)
			{
				LineCount++;
			}
		}

		int X = GroupX;
		int Y = GroupY;
		int W = BUDDY_DIALOG_WIDTH;
		int H = BUDDY_DIALOG_ITEM_HEIGHT * (1 + LineCount) + BUDDY_DIALOG_PADDING;

		Data = Dialog__DoItem(Data, Group->Caption, -1, BUDDY_DIALOG_BUTTON, BS_GROUPBOX, X, Y, W, H);
		ItemCount++;

		X += BUDDY_DIALOG_PADDING;
		Y += BUDDY_DIALOG_ITEM_HEIGHT;
		W -= 2 * BUDDY_DIALOG_PADDING + BUDDY_DIALOG_ICON_SIZE;

		Data = Dialog__DoItem(Data, Group->Icon, Group->IconId, BUDDY_DIALOG_LABEL, 0, X + BUDDY_DIALOG_PADDING, Y - BUDDY_DIALOG_PADDING, BUDDY_DIALOG_ICON_SIZE, BUDDY_DIALOG_ICON_SIZE);
		ItemCount++;

		X += BUDDY_DIALOG_ICON_SIZE;

		for (const Buddy_DialogItem* Item = Group->Items; Item->Text; Item++)
		{
			uint32_t Style = 0;
			if (Item->Control != BUDDY_DIALOG_LABEL)
			{
				Style |= WS_TABSTOP;
			}
			if (Item->Control == BUDDY_DIALOG_EDIT)
			{
				if (Item->Flags & BUDDY_DIALOG_READ_ONLY)
				{
					Style |= ES_READONLY;
				}
				Style |= WS_BORDER;
			}

			int ItemExtraY = (Item->Control == BUDDY_DIALOG_EDIT || Item->Width) ? -2 : 0;
			int ItemWidth = Item->Width ? Item->Width : W;

			Data = Dialog__DoItem(Data, Item->Text, Item->Id, Item->Control, Style, X, Y + ItemExtraY, ItemWidth, BUDDY_DIALOG_ITEM_HEIGHT);
			ItemCount++;

			if (Item->Flags & BUDDY_DIALOG_NEW_LINE)
			{
				X = GroupX + BUDDY_DIALOG_PADDING + BUDDY_DIALOG_ICON_SIZE;
				Y += BUDDY_DIALOG_ITEM_HEIGHT - ItemExtraY;
			}
			else
			{
				X += ItemWidth + BUDDY_DIALOG_PADDING / 2;
			}
		}

		GroupY = Y + BUDDY_DIALOG_PADDING;
	}

	*Template = (DLGTEMPLATE)
	{
		.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		.dwExtendedStyle = WS_EX_APPWINDOW,
		.cdit = ItemCount,
		.cx = BUDDY_DIALOG_PADDING + BUDDY_DIALOG_WIDTH + BUDDY_DIALOG_PADDING,
		.cy = GroupY,
	};

	Assert((void*)Data <= BufferEnd);
}

static HWND Buddy_CreateDialog(ScreenBuddy* Buddy)
{
	Buddy_DialogLayout DialogLayout =
	{
		.Title = BUDDY_TITLE,
		.Font = "Segoe UI",
		.FontSize = 9,
		.Groups = (Buddy_DialogGroup[])
		{
			{
				.Caption = "Share Your Screen",
 				.Icon = "\xEE\x85\x98",
				.IconId = BUDDY_ID_SHARE_ICON,
				.Items = (Buddy_DialogItem[])
				{
					{ "Use the following code to share your screen:",	0,						BUDDY_DIALOG_LABEL,		0,							BUDDY_DIALOG_NEW_LINE,	},
					{ "",												BUDDY_ID_SHARE_KEY,		BUDDY_DIALOG_EDIT,		BUDDY_DIALOG_KEY_WIDTH,		BUDDY_DIALOG_READ_ONLY,	},
					{ "\xEE\x85\xAF",									BUDDY_ID_SHARE_COPY,	BUDDY_DIALOG_BUTTON,	BUDDY_DIALOG_BUTTON_SMALL,							},
					{ "\xEE\x84\x97",									BUDDY_ID_SHARE_NEW,		BUDDY_DIALOG_BUTTON,	BUDDY_DIALOG_BUTTON_SMALL,	BUDDY_DIALOG_NEW_LINE,	},
					{ "Share",											BUDDY_ID_SHARE_BUTTON,	BUDDY_DIALOG_BUTTON,	BUDDY_DIALOG_BUTTON_WIDTH,	BUDDY_DIALOG_NEW_LINE,	},
					{ NULL },
				},
			},
			{
				.Caption = "Connect to Remote Computer",
				.Icon = "\xEE\x86\xA6",
				.IconId = BUDDY_ID_CONNECT_ICON,
				.Items = (Buddy_DialogItem[])
				{
					{ "Enter code for remote computer to connect to:",	0,							BUDDY_DIALOG_LABEL,		0,							BUDDY_DIALOG_NEW_LINE,	},
					{ "",												BUDDY_ID_CONNECT_KEY,		BUDDY_DIALOG_EDIT,		BUDDY_DIALOG_KEY_WIDTH,								},
					{ "\xEE\x85\xAD",									BUDDY_ID_CONNECT_PASTE,		BUDDY_DIALOG_BUTTON,	BUDDY_DIALOG_BUTTON_SMALL,	BUDDY_DIALOG_NEW_LINE,	},
					{ "Connect",										BUDDY_ID_CONNECT_BUTTON,	BUDDY_DIALOG_BUTTON,	BUDDY_DIALOG_BUTTON_WIDTH,	BUDDY_DIALOG_NEW_LINE,	},
					{ NULL },
				},
			},
			{ NULL },
		},
	};

	uint32_t Buffer[4096];
	Buddy_DoDialogLayout(&DialogLayout, Buffer, Buffer + ARRAYSIZE(Buffer));

	return CreateDialogIndirectParamW(NULL, (LPCDLGTEMPLATEW)Buffer, NULL, &Buddy_DialogProc, (LPARAM)Buddy);
}

static ScreenBuddy Buddy;

#ifndef NDEBUG
int WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int cmdshow)
#else
void WinMainCRTStartup()
#endif
{
	HR(CoInitializeEx(0, COINIT_APARTMENTTHREADED));
	HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));

	Buddy.HttpSession = WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	Assert(Buddy.HttpSession);

	LARGE_INTEGER Freq;
	QueryPerformanceFrequency(&Freq);
	Buddy.Freq = Freq.QuadPart;

	WSADATA WsaData;
	int WsaOk = WSAStartup(MAKEWORD(2, 2), &WsaData);
	Assert(WsaOk == 0);

	//

	Buddy_LoadConfig(&Buddy);

	//

	{
		UINT Flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifndef NDEBUG
		Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		HRESULT hr = D3D11CreateDevice(
			NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, Flags,
			(D3D_FEATURE_LEVEL[]) { D3D_FEATURE_LEVEL_11_0 }, 1,
			D3D11_SDK_VERSION, &Buddy.Device, NULL, &Buddy.Context);
		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot create D3D11 device!", L"Error", MB_ICONERROR);
			ExitProcess(0);
		}

		ID3D11InfoQueue* Info;
		if (Flags & D3D11_CREATE_DEVICE_DEBUG)
		{
			HR(ID3D11Device_QueryInterface(Buddy.Device, &IID_ID3D11InfoQueue, (void**)&Info));
			HR(ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE));
			HR(ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE));
			ID3D11InfoQueue_Release(Info);
		}

		ID3D11Multithread* Multithread;
		HR(ID3D11DeviceContext_QueryInterface(Buddy.Context, &IID_ID3D11Multithread, (void**)&Multithread));
		HR(ID3D11Multithread_SetMultithreadProtected(Multithread, TRUE));
		ID3D11Multithread_Release(Multithread);
	}

	// 

	WNDCLASSEXW WindowClass =
	{
		.cbSize = sizeof(WindowClass),
		.style = CS_HREDRAW | CS_VREDRAW,
		.lpfnWndProc = Buddy_WindowProc,
		.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)),
		.hCursor = LoadCursor(NULL, IDC_ARROW),
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = BUDDY_CLASS,
	};

	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	Buddy.Icon = WindowClass.hIcon;
	Assert(Buddy.Icon);

	//

	Buddy.DialogWindow = Buddy_CreateDialog(&Buddy);
	Assert(Buddy.DialogWindow);

	ShowWindow(Buddy.DialogWindow, SW_NORMAL);

	//

	for (;;)
	{
		MSG Message;
		BOOL Result = GetMessageW(&Message, NULL, 0, 0);
		if (Result == 0)
		{
			ExitProcess(0);
		}
		Assert(Result > 0);

		if (!IsDialogMessageW(Buddy.DialogWindow, &Message))
		{
			TranslateMessage(&Message);
			DispatchMessageW(&Message);
		}
	}
}
