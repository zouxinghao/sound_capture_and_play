//�������������߼�

#include<windows.h>

struct CommandLineSwitch
{
	//����������
	enum CommandLineSwitchType
	{
		SwitchTypeNone,
		//����������������
		SwitchTypeInteger,
		SwitchTypeString,
	};
	//!���������LPCWSTR �����˸��ģ����д���ע�⡪��
	LPCWSTR SwitchName;
	LPCWSTR SwitchHelp;

	CommandLineSwitchType SwitchType;
	//?����ΪʲôҪ��ָ���ָ������������
	void **SwitchValue;
	bool SwitchValueOptional;//���ܵĿ�ѡ����

};

bool ParseCommandLine(int argc, wchar_t *argv[], const CommandLineSwitch Switches[], size_t SwitchCount);