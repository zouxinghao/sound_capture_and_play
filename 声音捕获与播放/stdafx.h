//Ԥ����ͷ�ļ���������Ҫ���õ�ͷ�ļ���
//ģ�嶼���������

//���Ǿ��������ã����������ı�
#pragma once

#include<iostream>
#include<windows.h>
#include<strsafe.h>//��ȫʹ���ַ����Ŀ⡣������ʵû����ô����������Ҫ�һ��Ჹһ��C���԰�
#include<objbase.h>
#pragma warning(push)
#pragma warning(disable:4201)
#include<mmdeviceapi.h>
#include<audiopolicy.h>
#pragma warning(pop)


using namespace std;

extern bool isavaliable;
//������һ���ⲿ������������CoreAudioDemon���棬�����Ϊ�ܷ���ô��豸����������

//!����Ӧ���ǰ�ȫ�ͷſռ���ô������ǲ��Ǻ����Ϊʲô��ģ�塪��
template<class T> void SafeRelease(T **ppT) {
	if (*ppT) {
		(*ppT)->Release();
		*ppT = NULL;
	}
}
