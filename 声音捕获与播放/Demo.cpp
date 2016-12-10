#include"stdafx.h"
#include"WASAPICapture.h"
#include"CmdLine.h"


#include <functiondiscoverykeys.h>

int CaptureTime = 10;
int TargetLatency = 20;
wchar_t *OutEndpoint;

bool PickDevice(IMMDevice **Device) {
	HRESULT Hr;//�ɹ����أ�����ֵ>0��

	bool Value = true;
	//�����о��豸
	IMMDeviceEnumerator *devEnm = NULL;
	IMMDeviceCollection *devCol = NULL;

	//����һ��COM����(�ӿ�)
	Hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnm));
	if (Hr == 0) {
		printf("Unable to instantiate device enumerator: %x\n", Hr);
		Value = false;
		goto Exit;
	}

	IMMDevice *device = NULL;

	Hr = devEnm->EnumAudioEndpoints(
		eCapture,
		DEVICE_STATE_ACTIVE,  // dwStateMask���ն��豸��״̬����ǰ�Ѽ�����豸��
		&devCol  // ppDevices�������豸�ļ���
	);

	if (device == NULL) {
		Hr = devEnm->GetDefaultAudioEndpoint(eCapture, eMultimedia, &device);
		if (FAILED(Hr))
		{
			printf("Unable to get default device \n");
			Value = false;
			goto Exit;
		}
	}

	*Device = device;

Exit:
	SafeRelease(&devEnm);
	SafeRelease(&devCol);

	return Value;
}

int wmain() {
	int result = 0;
	IMMDevice *device = NULL;
	bool isDefaultDevice=true;
	ERole role=eMultimedia;

	printf("��Ƶ�ɼ��벥��\n");
	printf("�Ҳ��ܣ���Ȩ���ǹ������У�\n\n");

	if (!PickDevice(&device)) {
		result = -1;
		goto Exit;
	}

	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr))
	{
		printf("Unable to initialize COM: %x\n", hr);
		result = hr;
		goto Exit;
	}
	if (!PickDevice(&device)) {
		result = -1;
		goto Exit;
	}


	printf("Capture audio data for %d seconds\n", CaptureTime);

	CWASAPICapture *capturer = new (std::nothrow) CWASAPICapture(device, isDefaultDevice, role);
	if (capturer == NULL)
	{
		printf("Unable to allocate capturer\n");
		return -1;
	}

	if (capturer->Initialize(TargetLatency))
	{
		if (capturer->Start())
		{
			do
			{
				printf(".");
				Sleep(1000);
			} while (--CaptureTime);
			printf("\n");

			capturer->Stop();

			//
			//  Now shut down the capturer and release it we're done.
			//
			capturer->Shutdown();
			SafeRelease(&capturer);
		}
	}


Exit:
	SafeRelease(&device);
	CoUninitialize();
	system("pause");
	return 0;
}