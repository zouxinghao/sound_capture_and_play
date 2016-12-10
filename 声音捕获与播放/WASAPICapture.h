#pragma once
#include<MMDeviceAPI.h>
#include<Audioclient.h>
#include<Audiopolicy.h>
//有些敢于格式的定义要调用下面，本来已经放在stdafx.h下面了，但是出现红波浪感觉不是很爽
#include<windows.h>

//在Windows下使用线程
//!——这里直接粘贴了相关的库过来——
//！——有机会还是再配置一次吧——
#include "pthread.h"
#pragma comment (lib,"pthreadVC2")
//下面那句必须要有

#include<vector>
using std::vector;
//矢量：声音是矢量文件？

typedef struct _aduio_frame {
	char * buffer;//声音数据
	unsigned int length;//数据长度
}audio_frame;
typedef vector<audio_frame> audio_frame_list;


class CWASAPICapture:IAudioSessionEvents, IMMNotificationClient {
public:
	CWASAPICapture(IMMDevice *Endpoint, bool EnanbleStreamSwitch, ERole EndpointRole);
	~CWASAPICapture(void);
	LONG _RsfCount;
	//!——这两个函数具体用处，为什么要用虚函数——
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();

	bool Initialize(UINT32 EngineLatency);
	void Shutdown();
	void Start();
	void Stop();
	//
	//对设备的初始化
	//
	WAVEFORMAT *_OriginFormat;//设备的格式
	size_t _FrameSize;//样本的长度
	UINT32 BufferSize;//缓存的大小

	//设备的格式
	WAVEFORMAT *OriginFormat() { return _OriginFormat; }
	//设备的声道数
	WORD ChannelCount() { return _OriginFormat->nChannels; }
	//设备的频率数
	UINT32 SamplePerSecond() { return _OriginFormat->nSamplesPerSec; }
	//一个样本的长度
	size_t FrameSize() { return _FrameSize; }
	//一个样本的字节数
	//?——为什么不用nAvgBytesPerSec——
	UINT32 BytesPerSample() { return _OriginFormat->wBitsPerSample / 8; }

	IMMDevice *				_Endpoint;		//音频终端设备
	IAudioCaptureClient	*	_CaptureClient; //被捕获的对象
	IAudioClient *			_AudioClient;	//用于管理数据的对象

	HANDLE					_CaptureThread; //音频捕获线程
	HANDLE					_ShutdownEventp;//关闭事件
	HANDLE					_AudioSamplesReadyEvent;  // 音频数据准备好后系统向WASAPI客户端发送的事件

	//HANDLE仅仅是个32位的整形，只能表示进程ID
	//要想穿件线程，还需要：
	static DWORD _stdcall WASAPICaptureThread(LPVOID Context);//音频线程
	DWORD DoCaptureThread();						   //执行线程

	//---------------------------------------------------------------
	//					流切换的相关定义
	//				（目测目前用不到，视具体情况补充）
	//---------------------------------------------------------------


	//IAudioSessionEvents事件	如果不需要处理，返回S_OK
	STDMETHOD(OnChannelVolumeChanged)(DWORD /*ChannelCount*/, float /*NewChannelVolumes*/[], DWORD /*ChangedChannel*/, LPCGUID /*EventContext*/) { return S_OK; };
	STDMETHOD(OnDisplayNameChanged) (LPCWSTR /*NewDisplayName*/, LPCGUID /*EventContext*/) { return S_OK; };
	STDMETHOD(OnGroupingParamChanged) (LPCGUID /*NewGroupingParam*/, LPCGUID /*EventContext*/) { return S_OK; };
	STDMETHOD(OnIconPathChanged) (LPCWSTR /*NewIconPath*/, LPCGUID /*EventContext*/) { return S_OK; };
	// 会话连接断开
	STDMETHOD(OnSessionDisconnected) (AudioSessionDisconnectReason DisconnectReason);
	STDMETHOD(OnSimpleVolumeChanged) (float /*NewSimpleVolume*/, BOOL /*NewMute*/, LPCGUID /*EventContext*/) { return S_OK; }
	STDMETHOD(OnStateChanged) (AudioSessionState /*NewState*/) { return S_OK; };

	//-----------------------------------------------------------------
	//				IMMNoyification事件
	//				默认设备地改变
	//------------(目测用不到)————————————————————————

	bool InitializeAudioEngine();		//初始化音频设备（maybe是录音设备）

	//播放相关的成员和变量
	IMMDevice *					_PlayEndPoint;		//用于播放的终端设备
	IAudioClient *				_PlayAudioClient;	//用于管理播放数据流的对象
	IAudioRenderClient *        _RenderAuduioClient;//我理解为要被播放的对象

	HANDLE						_PlayThread;		//音乐播放线程
	HANDLE						_PlayShutdownEvent; //音乐播放线程关闭
	HANDLE						_PlaySampleReadyEvent;		//播放设备装备好了以后想WASAPI发送的信息

	static DWORD _stdcall WASAPIPlayThread(LPVOID Context);	//播放线程
	DWORD DoPlayThread();									//执行线程
	bool InitializePlayEngine();							//初始化播放设备

	//？——这个是用来干嘛的——
	audio_frame_list m_audio_list;
	pthread_mutex_t m_mutex;//互斥信号量
};