#pragma once
#include<MMDeviceAPI.h>
#include<Audioclient.h>
#include<Audiopolicy.h>
//��Щ���ڸ�ʽ�Ķ���Ҫ�������棬�����Ѿ�����stdafx.h�����ˣ����ǳ��ֺ첨�˸о����Ǻ�ˬ
#include<windows.h>

//��Windows��ʹ���߳�
//!��������ֱ��ճ������صĿ��������
//�������л��ỹ��������һ�ΰɡ���
#include "pthread.h"
#pragma comment (lib,"pthreadVC2")
//�����Ǿ����Ҫ��

#include<vector>
using std::vector;
//ʸ����������ʸ���ļ���

typedef struct _aduio_frame {
	char * buffer;//��������
	unsigned int length;//���ݳ���
}audio_frame;
typedef vector<audio_frame> audio_frame_list;


class CWASAPICapture:IAudioSessionEvents, IMMNotificationClient {
public:
	CWASAPICapture(IMMDevice *Endpoint, bool EnanbleStreamSwitch, ERole EndpointRole);
	~CWASAPICapture(void);
	LONG _RsfCount;
	//!�������������������ô���ΪʲôҪ���麯������
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();

	bool Initialize(UINT32 EngineLatency);
	void Shutdown();
	void Start();
	void Stop();
	//
	//���豸�ĳ�ʼ��
	//
	WAVEFORMAT *_OriginFormat;//�豸�ĸ�ʽ
	size_t _FrameSize;//�����ĳ���
	UINT32 BufferSize;//����Ĵ�С

	//�豸�ĸ�ʽ
	WAVEFORMAT *OriginFormat() { return _OriginFormat; }
	//�豸��������
	WORD ChannelCount() { return _OriginFormat->nChannels; }
	//�豸��Ƶ����
	UINT32 SamplePerSecond() { return _OriginFormat->nSamplesPerSec; }
	//һ�������ĳ���
	size_t FrameSize() { return _FrameSize; }
	//һ���������ֽ���
	//?����Ϊʲô����nAvgBytesPerSec����
	UINT32 BytesPerSample() { return _OriginFormat->wBitsPerSample / 8; }

	IMMDevice *				_Endpoint;		//��Ƶ�ն��豸
	IAudioCaptureClient	*	_CaptureClient; //������Ķ���
	IAudioClient *			_AudioClient;	//���ڹ������ݵĶ���

	HANDLE					_CaptureThread; //��Ƶ�����߳�
	HANDLE					_ShutdownEventp;//�ر��¼�
	HANDLE					_AudioSamplesReadyEvent;  // ��Ƶ����׼���ú�ϵͳ��WASAPI�ͻ��˷��͵��¼�

	//HANDLE�����Ǹ�32λ�����Σ�ֻ�ܱ�ʾ����ID
	//Ҫ�봩���̣߳�����Ҫ��
	static DWORD _stdcall WASAPICaptureThread(LPVOID Context);//��Ƶ�߳�
	DWORD DoCaptureThread();						   //ִ���߳�

	//---------------------------------------------------------------
	//					���л�����ض���
	//				��Ŀ��Ŀǰ�ò������Ӿ���������䣩
	//---------------------------------------------------------------


	//IAudioSessionEvents�¼�	�������Ҫ��������S_OK
	STDMETHOD(OnChannelVolumeChanged)(DWORD /*ChannelCount*/, float /*NewChannelVolumes*/[], DWORD /*ChangedChannel*/, LPCGUID /*EventContext*/) { return S_OK; };
	STDMETHOD(OnDisplayNameChanged) (LPCWSTR /*NewDisplayName*/, LPCGUID /*EventContext*/) { return S_OK; };
	STDMETHOD(OnGroupingParamChanged) (LPCGUID /*NewGroupingParam*/, LPCGUID /*EventContext*/) { return S_OK; };
	STDMETHOD(OnIconPathChanged) (LPCWSTR /*NewIconPath*/, LPCGUID /*EventContext*/) { return S_OK; };
	// �Ự���ӶϿ�
	STDMETHOD(OnSessionDisconnected) (AudioSessionDisconnectReason DisconnectReason);
	STDMETHOD(OnSimpleVolumeChanged) (float /*NewSimpleVolume*/, BOOL /*NewMute*/, LPCGUID /*EventContext*/) { return S_OK; }
	STDMETHOD(OnStateChanged) (AudioSessionState /*NewState*/) { return S_OK; };

	//-----------------------------------------------------------------
	//				IMMNoyification�¼�
	//				Ĭ���豸�ظı�
	//------------(Ŀ���ò���)������������������������������������������������

	bool InitializeAudioEngine();		//��ʼ����Ƶ�豸��maybe��¼���豸��

	//������صĳ�Ա�ͱ���
	IMMDevice *					_PlayEndPoint;		//���ڲ��ŵ��ն��豸
	IAudioClient *				_PlayAudioClient;	//���ڹ������������Ķ���
	IAudioRenderClient *        _RenderAuduioClient;//�����ΪҪ�����ŵĶ���

	HANDLE						_PlayThread;		//���ֲ����߳�
	HANDLE						_PlayShutdownEvent; //���ֲ����̹߳ر�
	HANDLE						_PlaySampleReadyEvent;		//�����豸װ�������Ժ���WASAPI���͵���Ϣ

	static DWORD _stdcall WASAPIPlayThread(LPVOID Context);	//�����߳�
	DWORD DoPlayThread();									//ִ���߳�
	bool InitializePlayEngine();							//��ʼ�������豸

	//�������������������ġ���
	audio_frame_list m_audio_list;
	pthread_mutex_t m_mutex;//�����ź���
};