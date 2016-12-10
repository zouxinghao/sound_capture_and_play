//用来拆解命令的逻辑

#include<windows.h>

struct CommandLineSwitch
{
	//命令行类型
	enum CommandLineSwitchType
	{
		SwitchTypeNone,
		//？——整数？——
		SwitchTypeInteger,
		SwitchTypeString,
	};
	//!——这里对LPCWSTR 进行了更改，如有错误注意——
	LPCWSTR SwitchName;
	LPCWSTR SwitchHelp;

	CommandLineSwitchType SwitchType;
	//?——为什么要用指针的指针来表明——
	void **SwitchValue;
	bool SwitchValueOptional;//功能的课选择性

};

bool ParseCommandLine(int argc, wchar_t *argv[], const CommandLineSwitch Switches[], size_t SwitchCount);