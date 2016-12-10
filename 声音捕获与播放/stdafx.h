//预编译头文件，所有需要引用的头文件和
//模板都会放在这里

//它们经常被调用，但不经常改变
#pragma once

#include<iostream>
#include<windows.h>
#include<strsafe.h>//安全使用字符串的库。。。其实没有这么看懂，还是要找机会补一下C语言啊
#include<objbase.h>
#pragma warning(push)
#pragma warning(disable:4201)
#include<mmdeviceapi.h>
#include<audiopolicy.h>
#pragma warning(pop)


using namespace std;

extern bool isavaliable;
//！——一个外部变量、定义在CoreAudioDemon里面，我理解为能否调用此设备——待更正

//!——应该是安全释放空间的用处，但是不是很理解为什么用模板——
template<class T> void SafeRelease(T **ppT) {
	if (*ppT) {
		(*ppT)->Release();
		*ppT = NULL;
	}
}
