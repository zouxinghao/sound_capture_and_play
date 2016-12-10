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
		AUDCLNT_SHAREMODE_SHARED,  // 打开设备的模式
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

	// 返回的buffersize指的是缓冲区最多可以存放多少帧的数据量
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

// EngineLatency参数用于设置IAudioClient对象的缓存持续时间
bool CWASAPICapture::Initialize(UINT32 EngineLatency)
{
	// 关闭设备的通知事件，用于关闭_CaptureThread线程
    _ShutdownEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (_ShutdownEvent == NULL)
    {
        printf("Unable to create shutdown event: %d.\n", GetLastError());
        return false;
    }

	// 接收音频数据的通知事件，音频数据准备好后由系统发出通知
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

	// 激活一个IAudioClient对象
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

	// 获取IAudioClient对象的音频格式
	hr = _AudioClient->GetMixFormat(&_MixFormat);
	if (FAILED(hr))
	{
		printf("Unable to get mix format on audio client: %x.\n", hr);
		return false;
	}

	// 计算一帧音频的长度
	_FrameSize = (_MixFormat->wBitsPerSample / 8) * _MixFormat->nChannels;

	// 音频的延迟时间，单位为毫秒，一般设定为20ms
    _EngineLatencyInMS = EngineLatency;

// 	WAVEFORMATEX wfx;
// 	::ZeroMemory(&wfx, sizeof(wfx));
// 	wfx.wFormatTag = WAVE_FORMAT_PCM;  // PCM声音
// 	wfx.nChannels = 1;    // 声道数
// 	wfx.wBitsPerSample = 16;        // 量化位数
// 	wfx.nSamplesPerSec = 8000;        // 采样频率
// 	wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * wfx.wBitsPerSample / 8; // 平均传输速率
// 	wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8; // 设置块对齐
// 	wfx.cbSize = 0;
// 
// 	WAVEFORMATEX * pWfxClosestMatch = NULL;
// 	// 为什么不能设置为8000Hz，Device Formats，https://msdn.microsoft.com/en-us/library/dd370811(v=vs.85).aspx
// 	HRESULT hr1 = _AudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &wfx, &pWfxClosestMatch);


	// 初始化IAudioClient对象
	// IAudioClient::Initialize method, https://msdn.microsoft.com/en-us/library/dd370875(v=vs.85).aspx
	hr = _AudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,  // ShareMode，打开设备的模式
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, // StreamFlags，以什么方式打开流。
		_EngineLatencyInMS * 10000, // hnsBufferDuration，音频缓存的持续时间，以100纳秒为单位。_EngineLatencyInMS的单位为毫秒，转换到纳秒要乘以1万
		0,           // hnsPeriodicity，设备周期，只能在独占模式下为非零值。在独占模式下，该参数为音频终端设备访问的连续缓冲区指定请求的调度周期。
		_MixFormat,  // pFormat，音频格式。
		NULL         // AudioSessionGuid，session的GUID，传入NULL等价于传递GUID_NULL。该值用于区别流属于哪个session。
	);

	if (FAILED(hr))
	{
		printf("Unable to initialize audio client: %x.\n", hr);
		return false;
	}

	// 获取IAudioClient对象的缓冲区长度
	hr = _AudioClient->GetBufferSize(&_BufferSize);
	if (FAILED(hr))
	{
		printf("Unable to get audio client buffer: %x. \n", hr);
		return false;
	}

	// 为IAudioClient设置通知的事件句柄，当音频缓冲准备完毕后，由系统发出此通知到WASAPI客户端
	hr = _AudioClient->SetEventHandle(_AudioSamplesReadyEvent);
	if (FAILED(hr))
	{
		printf("Unable to set ready event: %x.\n", hr);
		return false;
	}

	// 访问音频客户端对象的附加服务，即返回IAudioCaptureClient对象
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

	// 播放线程
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

	// 播放线程
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

// 接收音频数据线程
DWORD CWASAPICapture::WASAPICaptureThread(LPVOID Context)
{
    CWASAPICapture *capturer = static_cast<CWASAPICapture *>(Context);
    return capturer->DoCaptureThread();
}

// 接收音频数据线程执行体
DWORD CWASAPICapture::DoCaptureThread()
{
    bool stillPlaying = true;

	HANDLE waitArray[3] = { _ShutdownEvent, _StreamSwitchEvent, _AudioSamplesReadyEvent }; // 等待的事件
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
		// 把此线程与特殊的任务联系起来
		// Multimedia Class Scheduler Service, https://msdn.microsoft.com/en-us/ms684247(v=vs.85)
		// MMCSS服务运行于服务宿主Svchost.exe中，它可以自动提升音、视频播放的优先级，以防止其它软件过多占用播放软件应得到的CPU时间。
		// AvSetMmThreadCharacteristics就是通知MMCSS，这是一项关于“Audio”的特殊任务
        mmcssHandle = AvSetMmThreadCharacteristics(
			L"Audio", // TaskName，任务名称，要与注册表HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks中的某个子健相同，这是在win10上的注册表位置，在win7不一定相同
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
				&pData,  // ppData，音频数据缓存地址
				&framesAvailable, // pNumFramesToRead，
				&flags, // pdwFlags，返回的缓存状态标记
				NULL,   // pu64DevicePosition，指向写到数据包中第一帧的设备位置，该设备位置被表示为从流起始的帧数目。pu64DevicePosition和pu64QPCPosition用于获取数据包中第一帧音频数据的时间戳。为NULL表示客户端不需要该位置。
				NULL    // pu64QPCPosition，以100纳秒为单位，表示音频终端设备记录数据包中第一帧设备位置时的计数值。参考QueryPerformanceCounter和QueryPerformanceFrequency配合得到精确的时间计时。为NULL表示客户端不要求计数。
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
						// 静音。如果是网络语音通信，可以不予理会；如果是保存成文件，需要填充0
                        //  Fill 0s from the capture buffer to the output buffer.
                    }
                    else
                    {
                        //
                        //  Copy data from the audio engine buffer to the output buffer.
                        // 有效数据，此时数据存放在地址pData，framesAvailable表示帧数，数据长度则是framesAvailable*_FrameSize
						audio_frame buf;
						buf.length = framesAvailable*_FrameSize;
						buf.buffer = (char *)malloc(buf.length * sizeof(char));
						memcpy(buf.buffer, pData, buf.length);
						pthread_mutex_lock(&m_mutex);
						m_audio_list.push_back(buf);
						pthread_mutex_unlock(&m_mutex);
                    }
                }
				// 释放缓存
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
	// 获取数据流的格式（MSDN的英文解释并未出现mix这个单词，不知为什么要命名为mix format）
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

// 接收音频数据线程
DWORD CWASAPICapture::WASAPIRenderThread(LPVOID Context)
{
	CWASAPICapture *capturer = static_cast<CWASAPICapture *>(Context);
	return capturer->DoRenderThread();
}

// 接收音频数据线程执行体
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
				// 首先获取播放设备缓冲区还有多少帧未播放完
				hr = _RenderAudioClient->GetCurrentPadding(&padding);
				if (SUCCEEDED(hr))
				{
					//
					//  Calculate the number of frames available.  We'll render
					//  that many frames or the number of frames left in the buffer, whichever is smaller.
					//
					// 获取空余的缓冲区帧数
					framesAvailable = _RenderBufferSize - padding;

					//
					//  If the buffer at the head of the render buffer queue fits in the frames available, render it.  If we don't
					//  have enough room to fit the buffer, skip this pass - we will have enough room on the next pass.
					//
					// 写入数据
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
	IMMDeviceEnumerator *deviceEnumerator = NULL; // 用来列举音频终端设备
	IMMDeviceCollection *deviceCollection = NULL; // 代表一个音频终端设备的集合

	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator));
	if (FAILED(hr))
	{
		printf("Unable to instantiate device enumerator: %x\n", hr);
		retValue = false;
		goto Exit;
	}

	// 以角色eConsole打开音频播放设备终端
	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &_RenderEndpoint);
	if (FAILED(hr))
	{
		printf("Unable to get default device for role %d: %x\n", eConsole, hr);
		retValue = false;
		goto Exit;
	}

	// 关闭设备的通知事件，用于关闭_RenderThread线程
	_RenderShutdownEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (_RenderShutdownEvent == NULL)
	{
		printf("Unable to create rendershutdownevent event: %d.\n", GetLastError());
		return false;
	}

	// 当播放设备缓冲区准备好后发送的事件
	_RenderAudioSamplesReadyEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (_RenderAudioSamplesReadyEvent == NULL)
	{
		printf("Unable to create samples ready event: %d.\n", GetLastError());
		return false;
	}

	// 激活一个IAudioClient对象
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

	// 获取IAudioClient对象的音频格式
	hr = _RenderAudioClient->GetMixFormat(&_RenderMixFormat);
	if (FAILED(hr))
	{
		printf("Unable to get mix format on audio client: %x.\n", hr);
		return false;
	}

	_RenderFrameSize = _RenderMixFormat->nBlockAlign;

	// 初始化IAudioClient对象
	hr = _RenderAudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,  // ShareMode，打开设备的模式
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, // StreamFlags，以什么方式打开流。
		_EngineLatencyInMS * 10000, // hnsBufferDuration，音频缓存的持续时间，以100纳秒为单位。_EngineLatencyInMS的单位为毫秒，转换到纳秒要乘以1万
		0,    // hnsPeriodicity，设备周期，只能在独占模式下为非零值。在独占模式下，该参数为音频终端设备访问的连续缓冲区指定请求的调度周期。
		_RenderMixFormat,  // pFormat，音频格式。
		NULL  // AudioSessionGuid，session的GUID，传入NULL等价于传递GUID_NULL。该值用于区别流属于哪个session。
	);

	if (FAILED(hr))
	{
		printf("Unable to initialize audio client: %x.\n", hr);
		return false;
	}

	// 获取IAudioClient对象的缓冲区长度（单位为帧）
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

	// 访问音频客户端对象的附加服务，即返回IAudioRenderClient对象
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