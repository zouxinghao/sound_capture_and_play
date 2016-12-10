#include"stdafx.h"
#include"CmdLine.h"

//
//基于下面的选择来拆解需求、设置选择
//
bool ParseCommandLine(int argc, wchar_t *argv[], const CommandLineSwitch Switches[], size_t SwitchCount) {


	for (int i = 1; i < argc; i++)
	{
		if (argv[i][0]==L'-'||argv[i][0]==L'/')
		{
			size_t SwitchIndex;
			for ( SwitchIndex = 0; SwitchIndex < SwitchCount; SwitchIndex+=1)
			{
				size_t SwitchNameLenght = wcslen(Switches[SwitchIndex].SwitchName);
				if (_wcsnicmp(&argv[i][1], Switches[SwitchIndex].SwitchName, SwitchNameLenght)==0&&
					//!——再想想这一步的意思——
					(argv[i][SwitchNameLenght + 1] == L':' || argv[i][SwitchNameLenght + 1] == '\0'))
				{
					wchar_t *SwitchValue = NULL;
					if (Switches[SwitchIndex].SwitchType != CommandLineSwitch::SwitchTypeNone) {
						//看最后一个元素是不是":",如果是，用他来定义一个功能
						if (argv[i][SwitchNameLenght + 1] == L':') {
							SwitchValue = &argv[i][SwitchNameLenght + 2];
						}
						else if (i < argc)
						{
							//
							//  If the switch value isn't optional, the next argument
							//  must be the value.
							//
							if (!Switches[SwitchIndex].SwitchValueOptional)
							{
								SwitchValue = argv[i + 1];
								i += 1; // Skip the argument.
							}
							//
							//  Otherwise the switch value is optional, so check the next parameter.
							//
							//  If it's a switch, the user didn't specify a value, if it's not a switch
							//  the user DID specify a value.
							//
							else if (argv[i + 1][0] != L'-' && argv[i + 1][0] != L'/')
							{
								SwitchValue = argv[i + 1];
								i += 1; // Skip the argument.
							}
						}
						else if (!Switches[SwitchIndex].SwitchValueOptional)
						{
							printf("Invalid command line argument parsing option %S\n", Switches[SwitchIndex].SwitchName);
							return false;
						}
						switch (Switches[SwitchIndex].SwitchType)
						{
							//
							//  SwitchTypeNone switches take a boolean parameter indiating whether or not the parameter was present.
							//
						case CommandLineSwitch::SwitchTypeNone:
							*reinterpret_cast<bool *>(Switches[SwitchIndex].SwitchValue) = true;
							break;
							//
							//  SwitchTypeInteger switches take an integer parameter.
							//
						case CommandLineSwitch::SwitchTypeInteger:
						{
							wchar_t *endValue;
							long value = wcstoul(SwitchValue, &endValue, 0);
							if (value == ULONG_MAX || value == 0 || (*endValue != L'\0' && !iswspace(*endValue)))
							{
								printf("Command line switch %S expected an integer value, received %S", Switches[SwitchIndex].SwitchName, SwitchValue);
								return false;
							}
							*reinterpret_cast<long *>(Switches[SwitchIndex].SwitchValue) = value;
							break;
						}
						//
						//  SwitchTypeString switches take a string parameter - allocate a buffer for the string using operator new[].
						//
						case CommandLineSwitch::SwitchTypeString:
						{
							wchar_t ** switchLocation = reinterpret_cast<wchar_t **>(Switches[SwitchIndex].SwitchValue);
							//
							//  If the user didn't specify a value, set the location to NULL.
							//
							if (SwitchValue == NULL || *SwitchValue == '\0')
							{
								*switchLocation = NULL;
							}
							else
							{
								size_t switchLength = wcslen(SwitchValue) + 1;
								*switchLocation = new (std::nothrow) wchar_t[switchLength];
								if (*switchLocation == NULL)
								{
									printf("Unable to allocate memory for switch %S", Switches[SwitchIndex].SwitchName);
									return false;
								}

								HRESULT hr = StringCchCopy(*switchLocation, switchLength, SwitchValue);
								if (FAILED(hr))
								{
									printf("Unable to copy command line string %S to buffer\n", SwitchValue);
									return false;
								}
							}
							break;
						}
						default:
							break;
						}
						//  We've processed this command line switch, we can move to the next argument.
						//
						break;
					}
				}
					if (SwitchIndex == SwitchCount)
					{
						printf("unrecognized switch: %S", argv[i]);
						return false;
					}
					}
				}
			}
				return true;
}
		
	
