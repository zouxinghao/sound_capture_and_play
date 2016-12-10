// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved
//
#include "StdAfx.h"
#include <assert.h>
#include<Schedule.h>
#include <avrt.h>
#include "WASAPICapture.h"

//
//  A simple WASAPI Capture client.
//

// EnableStreamSwitch = isDefaultDevice
CWASAPICapture::CWASAPICapture(IMMDevice *Endpoint, bool EnableStreamSwitch, ERole EndpointRole) : 
    _RefCount(1),
    _Endpoint(Endpoint),
    _AudioClient(NULL),
    _CaptureClient(NULL),
    _CaptureThread(NULL),
    _ShutdownEvent(NULL),
    _MixFormat(NULL),
    _AudioSamplesReadyEvent(NULL),
    
    _EndpointRole(EndpointRole),
    _StreamSwitchEvent(NULL),
    _StreamSwitchCompleteEvent(NULL),
    _AudioSessionControl(NULL),
    _DeviceEnumerator(NULL),
    _InStreamSwitch(false),
	_RenderMixFormat(NULL),
	m_mutex(PTHREAD_MUTEX_INITIALIZER)
{
    _Endpoint->AddRef();    // Since we're holding a copy of the endpoint, take a reference to it.  It'll be released in Shutdown();
	pthread_mutex_init(&m_mutex, NULL);
}

//
//  Empty destructor - everything should be released in the Shutdown() call.
//
CWASAPICapture::~CWASAPICapture(void) 
{
	pthread_mutex_destroy(&m_mutex);
}


//
//  Initialize WASAPI in event driven mode, associate the audio client with our samples ready event handle, retrieve 
//  a capture client for the transport, create the capture thread and start the audio engine.
//
bool CWASAPICapture::InitializeAudioEngine()
{
    HRESULT hr = _AudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,  // ���豸��ģʽ
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
		_EngineLatencyInMS*10000,
		0,
		_MixFormat,
		NULL
	);

    if (FAILED(hr))
    {
        printf("Unable to initialize audio client: %x.\n", hr);
        return false;
    }

	// ���ص�buffersizeָ���ǻ����������Դ�Ŷ���֡��������
    hr = _AudioClient->GetBufferSize(&_BufferSize);
    if(FAILED(hr))
    {
        printf("Unable to get audio client buffer: %x. \n", hr);
        return false;
    }

	hr = _AudioClient->SetEventHandle(_AudioSamplesReadyEvent);
	if (FAILED(hr))
	{
		printf("Unable to set ready event: %x.\n", hr);
		return false;
	}

    hr = _AudioClient->GetService(IID_PPV_ARGS(&_CaptureClient));
    if (FAILED(hr))
    {
        printf("Unable to get new capture client: %x.\n", hr);
        return false;
    }

    return true;
}

// EngineLatency������������IAudioClient����Ļ������ʱ��
bool CWASAPICapture::Initialize(UINT32 EngineLatency)
{
	// �ر��豸��֪ͨ�¼������ڹر�_CaptureThread�߳�
    _ShutdownEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (_ShutdownEvent == NULL)
    {
        printf("Unable to create shutdown event: %d.\n", GetLastError());
        return false;
    }

	// ������Ƶ���ݵ�֪ͨ�¼�����Ƶ����׼���ú���ϵͳ����֪ͨ
    _AudioSamplesReadyEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (_AudioSamplesReadyEvent == NULL)
    {
        printf("Unable to create samples ready event: %d.\n", GetLastError());
        return false;
    }

    //
    //  Create our stream switch event- we want auto reset events that start in the not-signaled state.
    //  Note that we create this event even if we're not going to stream switch - that's because the event is used
    //  in the main loop of the capturer and thus it has to be set.
    //
    _StreamSwitchEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (_StreamSwitchEvent == NULL)
    {
        printf("Unable to create stream switch event: %d.\n", GetLastError());
        return false;
    }

	// ����һ��IAudioClient����
    HRESULT hr = _Endpoint->Activate(
		__uuidof(IAudioClient),
		CLSCTX_INPROC_SERVER,
		NULL,
		reinterpret_cast<void **>(&_AudioClient)
	);
    if (FAILED(hr))
    {
        printf("Unable to activate audio client: %x.\n", hr);
        return false;
    }

    hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator),
		NULL,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&_DeviceEnumerator)
	);
    if (FAILED(hr))
    {
        printf("Unable to instantiate device enumerator: %x\n", hr);
        return false;
    }

	// ��ȡIAudioClient�������Ƶ��ʽ
	hr = _AudioClient->GetMixFormat(&_MixFormat);
	if (FAILED(hr))
	{
		printf("Unable to get mix format on audio client: %x.\n", hr);
		return false;
	}

	// ����һ֡��Ƶ�ĳ���
	_FrameSize = (_MixFormat->wBitsPerSample / 8) * _MixFormat->nChannels;

	// ��Ƶ���ӳ�ʱ�䣬��λΪ���룬һ���趨Ϊ20ms
    _EngineLatencyInMS = EngineLatency;

// 	WAVEFORMATEX wfx;
// 	::ZeroMemory(&wfx, sizeof(wfx));
// 	wfx.wFormatTag = WAVE_FORMAT_PCM;  // PCM����
// 	wfx.nChannels = 1;    // ������
// 	wfx.wBitsPerSample = 16;        // ����λ��
// 	wfx.nSamplesPerSec = 8000;        // ����Ƶ��
// 	wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * wfx.wBitsPerSample / 8; // ƽ����������
// 	wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8; // ���ÿ����
// 	wfx.cbSize = 0;
// 
// 	WAVEFORMATEX * pWfxClosestMatch = NULL;
// 	// Ϊʲô��������Ϊ8000Hz��Device Formats��https://msdn.microsoft.com/en-us/library/dd370811(v=vs.85).aspx
// 	HRESULT hr1 = _AudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &wfx, &pWfxClosestMatch);


	// ��ʼ��IAudioClient����
	// IAudioClient::Initialize method, https://msdn.microsoft.com/en-us/library/dd370875(v=vs.85).aspx
	hr = _AudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,  // ShareMode�����豸��ģʽ
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, // StreamFlags����ʲô��ʽ������
		_EngineLatencyInMS * 10000, // hnsBufferDuration����Ƶ����ĳ���ʱ�䣬��100����Ϊ��λ��_EngineLatencyInMS�ĵ�λΪ���룬ת��������Ҫ����1��
		0,           // hnsPeriodicity���豸���ڣ�ֻ���ڶ�ռģʽ��Ϊ����ֵ���ڶ�ռģʽ�£��ò���Ϊ��Ƶ�ն��豸���ʵ�����������ָ������ĵ������ڡ�
		_MixFormat,  // pFormat����Ƶ��ʽ��
		NULL         // AudioSessionGuid��session��GUID������NULL�ȼ��ڴ���GUID_NULL����ֵ���������������ĸ�session��
	);

	if (FAILED(hr))
	{
		printf("Unable to initialize audio client: %x.\n", hr);
		return false;
	}

	// ��ȡIAudioClient����Ļ���������
	hr = _AudioClient->GetBufferSize(&_BufferSize);
	if (FAILED(hr))
	{
		printf("Unable to get audio client buffer: %x. \n", hr);
		return false;
	}

	// ΪIAudioClient����֪ͨ���¼����������Ƶ����׼����Ϻ���ϵͳ������֪ͨ��WASAPI�ͻ���
	hr = _AudioClient->SetEventHandle(_AudioSamplesReadyEvent);
	if (FAILED(hr))
	{
		printf("Unable to set ready event: %x.\n", hr);
		return false;
	}

	// ������Ƶ�ͻ��˶���ĸ��ӷ��񣬼�����IAudioCaptureClient����
	hr = _AudioClient->GetService(IID_PPV_ARGS(&_CaptureClient));
	if (FAILED(hr))
	{
		printf("Unable to get new capture client: %x.\n", hr);
		return false;
	}

    if (_EnableStreamSwitch)
    {
        if (!InitializeStreamSwitch())
        {
            return false;
        }
    }

	bool bInitRender = InitializeRenderEngine();

    return bInitRender;
}

//
//  Shut down the capture code and free all the resources.
//
void CWASAPICapture::Shutdown()
{
    if (_CaptureThread)
    {
        SetEvent(_ShutdownEvent);
        WaitForSingleObject(_CaptureThread, INFINITE);
        CloseHandle(_CaptureThread);
        _CaptureThread = NULL;
    }

    if (_ShutdownEvent)
    {
        CloseHandle(_ShutdownEvent);
        _ShutdownEvent = NULL;
    }
    if (_AudioSamplesReadyEvent)
    {
        CloseHandle(_AudioSamplesReadyEvent);
        _AudioSamplesReadyEvent = NULL;
    }
    if (_StreamSwitchEvent)
    {
        CloseHandle(_StreamSwitchEvent);
        _StreamSwitchEvent = NULL;
    }

    SafeRelease(&_Endpoint);
    SafeRelease(&_AudioClient);
    SafeRelease(&_CaptureClient);

    if (_MixFormat)
    {
        CoTaskMemFree(_MixFormat);
        _MixFormat = NULL;
    }

    if (_EnableStreamSwitch)
    {
        TerminateStreamSwitch();
    }

	// �����߳�
	if (_RenderThread)
	{
		SetEvent(_RenderShutdownEvent);
		WaitForSingleObject(_RenderThread, INFINITE);
		CloseHandle(_RenderThread);
		_RenderThread = NULL;
	}

	if (_RenderShutdownEvent)
	{
		CloseHandle(_RenderShutdownEvent);
		_RenderShutdownEvent = NULL;
	}

	if (_RenderAudioSamplesReadyEvent)
	{
		CloseHandle(_RenderAudioSamplesReadyEvent);
		_RenderAudioSamplesReadyEvent = NULL;
	}

	SafeRelease(&_RenderEndpoint);
	SafeRelease(&_RenderAudioClient);
	SafeRelease(&_RenderClient);

	if (_RenderMixFormat)
	{
		CoTaskMemFree(_RenderMixFormat);
		_RenderMixFormat = NULL;
	}
}


//
//  Start capturing...
//
bool CWASAPICapture::Start()
{
    HRESULT hr;
	_RenderThread = CreateThread(NULL, 0, WASAPIRenderThread, this, 0, NULL);
	if (_RenderThread == NULL)
	{
		printf("Unable to create render transport thread: %x.", GetLastError());
		return false;
	}

	//
	//  We're ready to go, start capturing!
	//
	hr = _RenderAudioClient->Start();
	if (FAILED(hr))
	{
		printf("Unable to start capture client: %x.\n", hr);
		return false;
	}

    //
    //  Now create the thread which is going to drive the capture.
    //
    _CaptureThread = CreateThread(NULL, 0, WASAPICaptureThread, this, 0, NULL);
    if (_CaptureThread == NULL)
    {
        printf("Unable to create transport thread: %x.", GetLastError());
        return false;
    }

    //
    //  We're ready to go, start capturing!
    //
    hr = _AudioClient->Start();
    if (FAILED(hr))
    {
        printf("Unable to start capture client: %x.\n", hr);
        return false;
    }

    return true;
}

//
//  Stop the capturer.
//
void CWASAPICapture::Stop()
{
    HRESULT hr;

    //
    //  Tell the capture thread to shut down, wait for the thread to complete then clean up all the stuff we 
    //  allocated in Start().
    //
    if (_ShutdownEvent)
    {
        SetEvent(_ShutdownEvent);
    }

    hr = _AudioClient->Stop();
    if (FAILED(hr))
    {
        printf("Unable to stop audio client: %x\n", hr);
    }

    if (_CaptureThread)
    {
        WaitForSingleObject(_CaptureThread, INFINITE);

        CloseHandle(_CaptureThread);
        _CaptureThread = NULL;
    }

	// �����߳�
	if (_RenderShutdownEvent)
	{
		SetEvent(_RenderShutdownEvent);
	}

	hr = _RenderAudioClient->Stop();
	if (FAILED(hr))
	{
		printf("Unable to stop render audio client: %x\n", hr);
	}

	if (_RenderThread)
	{
		WaitForSingleObject(_RenderThread, INFINITE);

		CloseHandle(_RenderThread);
		_RenderThread = NULL;
	}

	vector<audio_frame>::iterator it = m_audio_list.begin();
	for (; it != m_audio_list.end();)
	{
		free(it->buffer);
		it = m_audio_list.erase(it);
	}
	m_audio_list.clear();
}

// ������Ƶ�����߳�
DWORD CWASAPICapture::WASAPICaptureThread(LPVOID Context)
{
    CWASAPICapture *capturer = static_cast<CWASAPICapture *>(Context);
    return capturer->DoCaptureThread();
}

// ������Ƶ�����߳�ִ����
DWORD CWASAPICapture::DoCaptureThread()
{
    bool stillPlaying = true;

	HANDLE waitArray[3] = { _ShutdownEvent, _StreamSwitchEvent, _AudioSamplesReadyEvent }; // �ȴ����¼�
    HANDLE mmcssHandle = NULL;
    DWORD  mmcssTaskIndex = 0;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        printf("Unable to initialize COM in render thread: %x\n", hr);
        return hr;
    }

    if (!DisableMMCSS)
    {
		// AvSetMmThreadCharacteristics function, https://msdn.microsoft.com/en-us/ms681974(VS.85).aspx
		// �Ѵ��߳��������������ϵ����
		// Multimedia Class Scheduler Service, https://msdn.microsoft.com/en-us/ms684247(v=vs.85)
		// MMCSS���������ڷ�������Svchost.exe�У��������Զ�����������Ƶ���ŵ����ȼ����Է�ֹ�����������ռ�ò������Ӧ�õ���CPUʱ�䡣
		// AvSetMmThreadCharacteristics����֪ͨMMCSS������һ����ڡ�Audio������������
        mmcssHandle = AvSetMmThreadCharacteristics(
			L"Audio", // TaskName���������ƣ�Ҫ��ע���HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks�е�ĳ���ӽ���ͬ��������win10�ϵ�ע���λ�ã���win7��һ����ͬ
			&mmcssTaskIndex
		);
        if (mmcssHandle == NULL)
        {
            printf("Unable to enable MMCSS on capture thread: %d\n", GetLastError());
        }
    }
    while (stillPlaying)
    {
		DWORD waitResult = WaitForMultipleObjects(3, waitArray, FALSE, INFINITE);

        switch (waitResult)
        {
        case WAIT_OBJECT_0 + 0:     // _ShutdownEvent
            stillPlaying = false;       // We're done, exit the loop.
            break;
        case WAIT_OBJECT_0 + 1:     // _StreamSwitchEvent
            //
            //  We've received a stream switch request.
            //
            //  We need to stop the capturer, tear down the _AudioClient and _CaptureClient objects and re-create them on the new.
            //  endpoint if possible.  If this fails, abort the thread.
            //
            if (!HandleStreamSwitchEvent())
            {
                stillPlaying = false;
            }
            break;
		case WAIT_OBJECT_0 + 2:     // _AudioSamplesReadyEvent
            //
            //  We need to retrieve the next buffer of samples from the audio capturer.
            //
            BYTE *pData;
            UINT32 framesAvailable;
            DWORD  flags;

            //
            //  Find out how much capture data is available.  We need to make sure we don't run over the length
            //  of our capture buffer.  We'll discard any samples that don't fit in the buffer.
            // IAudioCaptureClient::GetBuffer method, https://msdn.microsoft.com/en-us/library/dd370859(v=vs.85).aspx
            hr = _CaptureClient->GetBuffer(
				&pData,  // ppData����Ƶ���ݻ����ַ
				&framesAvailable, // pNumFramesToRead��
				&flags, // pdwFlags�����صĻ���״̬���
				NULL,   // pu64DevicePosition��ָ��д�����ݰ��е�һ֡���豸λ�ã����豸λ�ñ���ʾΪ������ʼ��֡��Ŀ��pu64DevicePosition��pu64QPCPosition���ڻ�ȡ���ݰ��е�һ֡��Ƶ���ݵ�ʱ�����ΪNULL��ʾ�ͻ��˲���Ҫ��λ�á�
				NULL    // pu64QPCPosition����100����Ϊ��λ����ʾ��Ƶ�ն��豸��¼���ݰ��е�һ֡�豸λ��ʱ�ļ���ֵ���ο�QueryPerformanceCounter��QueryPerformanceFrequency��ϵõ���ȷ��ʱ���ʱ��ΪNULL��ʾ�ͻ��˲�Ҫ�������
			);
            if (SUCCEEDED(hr))
            {
                if (framesAvailable != 0)
                {
                    //
                    //  The flags on capture tell us information about the data.
                    //
                    //  We only really care about the silent flag since we want to put frames of silence into the buffer
                    //  when we receive silence.  We rely on the fact that a logical bit 0 is silence for both float and int formats.
                    //
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)  
                    {
						// �������������������ͨ�ţ����Բ�����᣻����Ǳ�����ļ�����Ҫ���0
                        //  Fill 0s from the capture buffer to the output buffer.
                    }
                    else
                    {
                        //
                        //  Copy data from the audio engine buffer to the output buffer.
                        // ��Ч���ݣ���ʱ���ݴ���ڵ�ַpData��framesAvailable��ʾ֡�������ݳ�������framesAvailable*_FrameSize
						audio_frame buf;
						buf.length = framesAvailable*_FrameSize;
						buf.buffer = (char *)malloc(buf.length * sizeof(char));
						memcpy(buf.buffer, pData, buf.length);
						pthread_mutex_lock(&m_mutex);
						m_audio_list.push_back(buf);
						pthread_mutex_unlock(&m_mutex);
                    }
                }
				// �ͷŻ���
                hr = _CaptureClient->ReleaseBuffer(framesAvailable);
                if (FAILED(hr))
                {
                    printf("Unable to release capture buffer: %x!\n", hr);
                }
            }
            break;
        }
    }
    if (!DisableMMCSS)
    {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }

    CoUninitialize();
    return 0;
}


//
//  Initialize the stream switch logic.
//
bool CWASAPICapture::InitializeStreamSwitch()
{
    HRESULT hr = _AudioClient->GetService(IID_PPV_ARGS(&_AudioSessionControl));
    if (FAILED(hr))
    {
        printf("Unable to retrieve session control: %x\n", hr);
        return false;
    }

    //
    //  Create the stream switch complete event- we want a manual reset event that starts in the not-signaled state.
    //
    _StreamSwitchCompleteEvent = CreateEventEx(NULL, NULL, CREATE_EVENT_INITIAL_SET | CREATE_EVENT_MANUAL_RESET, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (_StreamSwitchCompleteEvent == NULL)
    {
        printf("Unable to create stream switch event: %d.\n", GetLastError());
        return false;
    }
    //
    //  Register for session and endpoint change notifications.  
    //
    //  A stream switch is initiated when we receive a session disconnect notification or we receive a default device changed notification.
    //
    hr = _AudioSessionControl->RegisterAudioSessionNotification(this);
    if (FAILED(hr))
    {
        printf("Unable to register for stream switch notifications: %x\n", hr);
        return false;
    }

    hr = _DeviceEnumerator->RegisterEndpointNotificationCallback(this);
    if (FAILED(hr))
    {
        printf("Unable to register for stream switch notifications: %x\n", hr);
        return false;
    }

    return true;
}

void CWASAPICapture::TerminateStreamSwitch()
{
    HRESULT hr = _AudioSessionControl->UnregisterAudioSessionNotification(this);
    if (FAILED(hr))
    {
        printf("Unable to unregister for session notifications: %x\n", hr);
    }

    _DeviceEnumerator->UnregisterEndpointNotificationCallback(this);
    if (FAILED(hr))
    {
        printf("Unable to unregister for endpoint notifications: %x\n", hr);
    }

    if (_StreamSwitchCompleteEvent)
    {
        CloseHandle(_StreamSwitchCompleteEvent);
        _StreamSwitchCompleteEvent = NULL;
    }

    SafeRelease(&_AudioSessionControl);
    SafeRelease(&_DeviceEnumerator);
}

//
//  Handle the stream switch.
//
//  When a stream switch happens, we want to do several things in turn:
//
//  1) Stop the current capturer.
//  2) Release any resources we have allocated (the _AudioClient, _AudioSessionControl (after unregistering for notifications) and 
//        _CaptureClient).
//  3) Wait until the default device has changed (or 500ms has elapsed).  If we time out, we need to abort because the stream switch can't happen.
//  4) Retrieve the new default endpoint for our role.
//  5) Re-instantiate the audio client on that new endpoint.  
//  6) Retrieve the mix format for the new endpoint.  If the mix format doesn't match the old endpoint's mix format, we need to abort because the stream
//      switch can't happen.
//  7) Re-initialize the _AudioClient.
//  8) Re-register for session disconnect notifications and reset the stream switch complete event.
//
bool CWASAPICapture::HandleStreamSwitchEvent()
{
    HRESULT hr;

    assert(_InStreamSwitch);
    //
    //  Step 1.  Stop capturing.
    //
    hr = _AudioClient->Stop();
    if (FAILED(hr))
    {
        printf("Unable to stop audio client during stream switch: %x\n", hr);
        goto ErrorExit;
    }

    //
    //  Step 2.  Release our resources.  Note that we don't release the mix format, we need it for step 6.
    //
    hr = _AudioSessionControl->UnregisterAudioSessionNotification(this);
    if (FAILED(hr))
    {
        printf("Unable to stop audio client during stream switch: %x\n", hr);
        goto ErrorExit;
    }

    SafeRelease(&_AudioSessionControl);
    SafeRelease(&_CaptureClient);
    SafeRelease(&_AudioClient);
    SafeRelease(&_Endpoint);

    //
    //  Step 3.  Wait for the default device to change.
    //
    //  There is a race between the session disconnect arriving and the new default device 
    //  arriving (if applicable).  Wait the shorter of 500 milliseconds or the arrival of the 
    //  new default device, then attempt to switch to the default device.  In the case of a 
    //  format change (i.e. the default device does not change), we artificially generate  a
    //  new default device notification so the code will not needlessly wait 500ms before 
    //  re-opening on the new format.  (However, note below in step 6 that in this SDK 
    //  sample, we are unlikely to actually successfully absorb a format change, but a 
    //  real audio application implementing stream switching would re-format their 
    //  pipeline to deliver the new format).  
    //
    DWORD waitResult = WaitForSingleObject(_StreamSwitchCompleteEvent, 500);
    if (waitResult == WAIT_TIMEOUT)
    {
        printf("Stream switch timeout - aborting...\n");
        goto ErrorExit;
    }

    //
    //  Step 4.  If we can't get the new endpoint, we need to abort the stream switch.  If there IS a new device,
    //          we should be able to retrieve it.
    //
    hr = _DeviceEnumerator->GetDefaultAudioEndpoint(eCapture, _EndpointRole, &_Endpoint);
    if (FAILED(hr))
    {
        printf("Unable to retrieve new default device during stream switch: %x\n", hr);
        goto ErrorExit;
    }
    //
    //  Step 5 - Re-instantiate the audio client on the new endpoint.
    //
    hr = _Endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, NULL, reinterpret_cast<void **>(&_AudioClient));
    if (FAILED(hr))
    {
        printf("Unable to activate audio client on the new endpoint: %x.\n", hr);
        goto ErrorExit;
    }
    //
    //  Step 6 - Retrieve the new mix format.
    //
    WAVEFORMATEX *wfxNew;
	// ��ȡ�������ĸ�ʽ��MSDN��Ӣ�Ľ��Ͳ�δ����mix������ʣ���֪ΪʲôҪ����Ϊmix format��
    hr = _AudioClient->GetMixFormat(&wfxNew);
    if (FAILED(hr))
    {
        printf("Unable to retrieve mix format for new audio client: %x.\n", hr);
        goto ErrorExit;
    }

    //
    //  Note that this is an intentionally naive comparison.  A more sophisticated comparison would
    //  compare the sample rate, channel count and format and apply the appropriate conversions into the capture pipeline.
    //
    if (memcmp(_MixFormat, wfxNew, sizeof(WAVEFORMATEX) + wfxNew->cbSize) != 0)
    {
        printf("New mix format doesn't match old mix format.  Aborting.\n");
        CoTaskMemFree(wfxNew);
        goto ErrorExit;
    }
    CoTaskMemFree(wfxNew);

    //
    //  Step 7:  Re-initialize the audio client.
    //
    if (!InitializeAudioEngine())
    {
        goto ErrorExit;
    }

    //
    //  Step 8: Re-register for session disconnect notifications.
    //
    hr = _AudioClient->GetService(IID_PPV_ARGS(&_AudioSessionControl));
    if (FAILED(hr))
    {
        printf("Unable to retrieve session control on new audio client: %x\n", hr);
        goto ErrorExit;
    }
    hr = _AudioSessionControl->RegisterAudioSessionNotification(this);
    if (FAILED(hr))
    {
        printf("Unable to retrieve session control on new audio client: %x\n", hr);
        goto ErrorExit;
    }

    //
    //  Reset the stream switch complete event because it's a manual reset event.
    //
    ResetEvent(_StreamSwitchCompleteEvent);
    //
    //  And we're done.  Start capturing again.
    //
    hr = _AudioClient->Start();
    if (FAILED(hr))
    {
        printf("Unable to start the new audio client: %x\n", hr);
        goto ErrorExit;
    }

    _InStreamSwitch = false;
    return true;

ErrorExit:
    _InStreamSwitch = false;
    return false;
}

//
//  Called when an audio session is disconnected.  
//
//  When a session is disconnected because of a device removal or format change event, we just want 
//  to let the capture thread know that the session's gone away
//
HRESULT CWASAPICapture::OnSessionDisconnected(AudioSessionDisconnectReason DisconnectReason)
{
    if (DisconnectReason == DisconnectReasonDeviceRemoval)
    {
        //
        //  The stream was disconnected because the device we're capturing to was removed.
        //
        //  We want to reset the stream switch complete event (so we'll block when the HandleStreamSwitchEvent function
        //  waits until the default device changed event occurs).
        //
        //  Note that we don't set the _StreamSwitchCompleteEvent - that will be set when the OnDefaultDeviceChanged event occurs.
        //
        _InStreamSwitch = true;
        SetEvent(_StreamSwitchEvent);
    }
    if (DisconnectReason == DisconnectReasonFormatChanged)
    {
        //
        //  The stream was disconnected because the format changed on our capture device.
        //
        //  We want to flag that we're in a stream switch and then set the stream switch event (which breaks out of the capturer).  We also
        //  want to set the _StreamSwitchCompleteEvent because we're not going to see a default device changed event after this.
        //
        _InStreamSwitch = true;
        SetEvent(_StreamSwitchEvent);
        SetEvent(_StreamSwitchCompleteEvent);
    }
    return S_OK;
}
//
//  Called when the default capture device changed.  We just want to set an event which lets the stream switch logic know that it's ok to 
//  continue with the stream switch.
//
HRESULT CWASAPICapture::OnDefaultDeviceChanged(EDataFlow Flow, ERole Role, LPCWSTR /*NewDefaultDeviceId*/)
{
    if (Flow == eCapture && Role == _EndpointRole)
    {
        //
        //  The default capture device for our configuredf role was changed.  
        //
        //  If we're not in a stream switch already, we want to initiate a stream switch event.  
        //  We also we want to set the stream switch complete event.  That will signal the capture thread that it's ok to re-initialize the
        //  audio capturer.
        //
        if (!_InStreamSwitch)
        {
            _InStreamSwitch = true;
            SetEvent(_StreamSwitchEvent);
        }
        SetEvent(_StreamSwitchCompleteEvent);
    }
    return S_OK;
}

//
//  IUnknown
//
HRESULT CWASAPICapture::QueryInterface(REFIID Iid, void **Object)
{
    if (Object == NULL)
    {
        return E_POINTER;
    }
    *Object = NULL;

    if (Iid == IID_IUnknown)
    {
        *Object = static_cast<IUnknown *>(static_cast<IAudioSessionEvents *>(this));
        AddRef();
    }
    else if (Iid == __uuidof(IMMNotificationClient))
    {
        *Object = static_cast<IMMNotificationClient *>(this);
        AddRef();
    }
    else if (Iid == __uuidof(IAudioSessionEvents))
    {
        *Object = static_cast<IAudioSessionEvents *>(this);
        AddRef();
    }
    else
    {
        return E_NOINTERFACE;
    }
    return S_OK;
}

ULONG CWASAPICapture::AddRef()
{
    return InterlockedIncrement(&_RefCount);
}

ULONG CWASAPICapture::Release()
{
    ULONG returnValue = InterlockedDecrement(&_RefCount);
    if (returnValue == 0)
    {
        delete this;
    }
    return returnValue;
}

// ������Ƶ�����߳�
DWORD CWASAPICapture::WASAPIRenderThread(LPVOID Context)
{
	CWASAPICapture *capturer = static_cast<CWASAPICapture *>(Context);
	return capturer->DoRenderThread();
}

// ������Ƶ�����߳�ִ����
DWORD CWASAPICapture::DoRenderThread()
{
	bool stillPlaying = true;
	HANDLE waitArray[2] = { _RenderShutdownEvent, _RenderAudioSamplesReadyEvent };
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr))
	{
		printf("Unable to initialize COM in render thread: %x\n", hr);
		return hr;
	}

	while (stillPlaying)
	{
		DWORD waitResult = WaitForMultipleObjects(2, waitArray, FALSE, INFINITE);
		switch (waitResult)
		{
		case WAIT_OBJECT_0 + 0:     // _ShutdownEvent
			stillPlaying = false;       // We're done, exit the loop.
			break;
		case WAIT_OBJECT_0 + 1:
			if (m_audio_list.size() > 0)
			{
				pthread_mutex_lock(&m_mutex);
				BYTE *pData;
				UINT32 padding;
				UINT32 framesAvailable;

				vector<audio_frame>::iterator it = m_audio_list.begin();
				//
				//  We want to find out how much of the buffer *isn't* available (is padding).
				//
				// ���Ȼ�ȡ�����豸���������ж���֡δ������
				hr = _RenderAudioClient->GetCurrentPadding(&padding);
				if (SUCCEEDED(hr))
				{
					//
					//  Calculate the number of frames available.  We'll render
					//  that many frames or the number of frames left in the buffer, whichever is smaller.
					//
					// ��ȡ����Ļ�����֡��
					framesAvailable = _RenderBufferSize - padding;

					//
					//  If the buffer at the head of the render buffer queue fits in the frames available, render it.  If we don't
					//  have enough room to fit the buffer, skip this pass - we will have enough room on the next pass.
					//
					// д������
					if (it->length <= framesAvailable *_RenderFrameSize)
					{

						UINT32 framesToWrite = it->length / _RenderFrameSize;
						hr = _RenderClient->GetBuffer(framesToWrite, &pData);
						if (SUCCEEDED(hr))
						{
							memcpy(pData, it->buffer, framesToWrite*_RenderFrameSize);
							hr = _RenderClient->ReleaseBuffer(framesToWrite, 0);
							if (!SUCCEEDED(hr))
							{
								printf("Unable to release buffer: %x\n", hr);
								stillPlaying = false;
							}
						}
						else
						{
							printf("Unable to release buffer: %x\n", hr);
							stillPlaying = false;
						}
						//
						//  We're done with this set of samples, free it.
						//
						free(it->buffer);
						m_audio_list.erase(it);
					}
				}
				pthread_mutex_unlock(&m_mutex);
			}
			break;
		}
	}

	CoUninitialize();
	return 0;
}

bool CWASAPICapture::InitializeRenderEngine()
{
	HRESULT hr;
	bool retValue = true;
	IMMDeviceEnumerator *deviceEnumerator = NULL; // �����о���Ƶ�ն��豸
	IMMDeviceCollection *deviceCollection = NULL; // ����һ����Ƶ�ն��豸�ļ���

	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator));
	if (FAILED(hr))
	{
		printf("Unable to instantiate device enumerator: %x\n", hr);
		retValue = false;
		goto Exit;
	}

	// �Խ�ɫeConsole����Ƶ�����豸�ն�
	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &_RenderEndpoint);
	if (FAILED(hr))
	{
		printf("Unable to get default device for role %d: %x\n", eConsole, hr);
		retValue = false;
		goto Exit;
	}

	// �ر��豸��֪ͨ�¼������ڹر�_RenderThread�߳�
	_RenderShutdownEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (_RenderShutdownEvent == NULL)
	{
		printf("Unable to create rendershutdownevent event: %d.\n", GetLastError());
		return false;
	}

	// �������豸������׼���ú��͵��¼�
	_RenderAudioSamplesReadyEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (_RenderAudioSamplesReadyEvent == NULL)
	{
		printf("Unable to create samples ready event: %d.\n", GetLastError());
		return false;
	}

	// ����һ��IAudioClient����
	hr = _RenderEndpoint->Activate(
		__uuidof(IAudioClient),
		CLSCTX_INPROC_SERVER,
		NULL,
		reinterpret_cast<void **>(&_RenderAudioClient)
	);
	if (FAILED(hr))
	{
		printf("Unable to activate audio client: %x.\n", hr);
		return false;
	}

	// ��ȡIAudioClient�������Ƶ��ʽ
	hr = _RenderAudioClient->GetMixFormat(&_RenderMixFormat);
	if (FAILED(hr))
	{
		printf("Unable to get mix format on audio client: %x.\n", hr);
		return false;
	}

	_RenderFrameSize = _RenderMixFormat->nBlockAlign;

	// ��ʼ��IAudioClient����
	hr = _RenderAudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,  // ShareMode�����豸��ģʽ
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, // StreamFlags����ʲô��ʽ������
		_EngineLatencyInMS * 10000, // hnsBufferDuration����Ƶ����ĳ���ʱ�䣬��100����Ϊ��λ��_EngineLatencyInMS�ĵ�λΪ���룬ת��������Ҫ����1��
		0,    // hnsPeriodicity���豸���ڣ�ֻ���ڶ�ռģʽ��Ϊ����ֵ���ڶ�ռģʽ�£��ò���Ϊ��Ƶ�ն��豸���ʵ�����������ָ������ĵ������ڡ�
		_RenderMixFormat,  // pFormat����Ƶ��ʽ��
		NULL  // AudioSessionGuid��session��GUID������NULL�ȼ��ڴ���GUID_NULL����ֵ���������������ĸ�session��
	);

	if (FAILED(hr))
	{
		printf("Unable to initialize audio client: %x.\n", hr);
		return false;
	}

	// ��ȡIAudioClient����Ļ��������ȣ���λΪ֡��
	hr = _RenderAudioClient->GetBufferSize(&_RenderBufferSize);
	if (FAILED(hr))
	{
		printf("Unable to get audio client buffer: %x. \n", hr);
		return false;
	}

	hr = _RenderAudioClient->SetEventHandle(_RenderAudioSamplesReadyEvent);
	if (FAILED(hr))
	{
		printf("Unable to set ready event: %x.\n", hr);
		return false;
	}

	// ������Ƶ�ͻ��˶���ĸ��ӷ��񣬼�����IAudioRenderClient����
	hr = _RenderAudioClient->GetService(IID_PPV_ARGS(&_RenderClient));
	if (FAILED(hr))
	{
		printf("Unable to get new capture client: %x.\n", hr);
		return false;
	}

	retValue = true;
Exit:
	SafeRelease(&deviceCollection);
	SafeRelease(&deviceEnumerator);

	return retValue;
}