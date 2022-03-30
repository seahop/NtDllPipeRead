#include <stdio.h>
#include <windows.h>
#pragma warning(disable : 4996)

typedef struct _BackgroundConsoleInstanceStruct
{
	char szInstanceName[128];
	HANDLE hConsoleProcess;
	HANDLE hConsoleInputPipe;
}BackgroundConsoleInstanceStruct;

typedef struct _CommandOutput_StoreDataParamStruct
{
	BYTE* pOutputPtr;
	DWORD dwMaxOutputSize;
	DWORD dwTotalSize;
}CommandOutput_StoreDataParamStruct;

DWORD BackgroundConsole_Create(const char* pInstanceName, BackgroundConsoleInstanceStruct* pBackgroundConsoleInstance)
{
	PROCESS_INFORMATION ProcessInfo;
	STARTUPINFO StartupInfo;
	char szConsoleInputPipeName[512];
	char szLaunchCmd[1024];
	BackgroundConsoleInstanceStruct BackgroundConsoleInstance;
	HANDLE hConsoleInputPipe;

	// create console input pipe
	memset(szConsoleInputPipeName, 0, sizeof(szConsoleInputPipeName));
	_snprintf(szConsoleInputPipeName, sizeof(szConsoleInputPipeName) - 1, "\\\\.\\pipe\\%s", pInstanceName);
	hConsoleInputPipe = CreateNamedPipeA(szConsoleInputPipeName, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);

	if (hConsoleInputPipe == INVALID_HANDLE_VALUE)
	{
		// error
		printf("Error creaing pipe or invalid handle\n");
		return 1;
	};

	// initialise startupinfo
	memset(&StartupInfo, 0, sizeof(StartupInfo));
	StartupInfo.cb = sizeof(StartupInfo);
	StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
	StartupInfo.wShowWindow = SW_HIDE;

	// create launch cmd
	printf("allocating memory\n");
	memset(szLaunchCmd, 0, sizeof(szLaunchCmd));
	_snprintf(szLaunchCmd, sizeof(szLaunchCmd) - 1, "cmd /c cmd < %s\0", szConsoleInputPipeName);

	// launch cmd.exe
	printf("Launching cmd.exe\n");
	if (CreateProcessA(NULL, szLaunchCmd, NULL, NULL, 0, CREATE_NEW_CONSOLE, NULL, NULL, &StartupInfo, &ProcessInfo) == 0)
	{
		// error
		printf("Error creating process\n");
		CloseHandle(hConsoleInputPipe);
		return 1;
	}

	// close thread handle
	CloseHandle(ProcessInfo.hThread);

	// wait for cmd.exe to connect to input pipe
	if (ConnectNamedPipe(hConsoleInputPipe, NULL) == 0)
	{
		// error
		CloseHandle(hConsoleInputPipe);
		CloseHandle(ProcessInfo.hProcess);
		printf("Error connecting to pipe\n");
		return 1;
	}

	// store background console entry data
	memset((void*)&BackgroundConsoleInstance, 0, sizeof(BackgroundConsoleInstance));
	strncpy(BackgroundConsoleInstance.szInstanceName, pInstanceName, sizeof(BackgroundConsoleInstance.szInstanceName) - 1);
	BackgroundConsoleInstance.hConsoleProcess = ProcessInfo.hProcess;
	BackgroundConsoleInstance.hConsoleInputPipe = hConsoleInputPipe;
	memcpy((void*)pBackgroundConsoleInstance, (void*)&BackgroundConsoleInstance, sizeof(BackgroundConsoleInstance));

	return 0;
}

DWORD BackgroundConsole_Close(BackgroundConsoleInstanceStruct* pBackgroundConsoleInstance)
{
	// close console input pipe
	CloseHandle(pBackgroundConsoleInstance->hConsoleInputPipe);

	// wait for console process to end
	WaitForSingleObject(pBackgroundConsoleInstance->hConsoleProcess, INFINITE);
	CloseHandle(pBackgroundConsoleInstance->hConsoleProcess);

	return 0;
}

DWORD BackgroundConsole_Exec(BackgroundConsoleInstanceStruct* pBackgroundConsoleInstance, char* pCommand, DWORD(*pCommandOutput)(BYTE* pBufferData, DWORD dwBufferLength, BYTE* pParam), BYTE* pCommandOutputParam)
{
	char szWriteCommand[2048];
	char szCommandOutputPipeName[512];
	HANDLE hCommandOutputPipe = NULL;
	BYTE bReadBuffer[1024];
	DWORD dwBytesRead = 0;
	const char* pInstanceName = "abc123\0";

	// create output pipe
	memset(szCommandOutputPipeName, 0, sizeof(szCommandOutputPipeName));
	_snprintf(szCommandOutputPipeName, sizeof(szCommandOutputPipeName) - 1, "\\\\.\\pipe\\%s", pInstanceName);
	hCommandOutputPipe = CreateNamedPipeA(szCommandOutputPipeName, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
	if (hCommandOutputPipe == INVALID_HANDLE_VALUE)
	{
		// error
		printf("error on output pipe\n");
		return 1;
	}
	// write command to console
	memset(szWriteCommand, 0, sizeof(szWriteCommand));
	_snprintf(szWriteCommand, sizeof(szWriteCommand) - 1, "%s > %s\n", pCommand, szCommandOutputPipeName);
	if (WriteFile(pBackgroundConsoleInstance->hConsoleInputPipe, szWriteCommand, strlen(szWriteCommand), NULL, NULL) == 0)
	{
		// error
		CloseHandle(hCommandOutputPipe);
		return 1;
	}

	// wait for target to connect to output pipe
	if (ConnectNamedPipe(hCommandOutputPipe, NULL) == 0)
	{
		// error
		CloseHandle(hCommandOutputPipe);
		return 1;
	}

	// get data from output pipe
	for (;;)
	{
		// read data from stdout pipe (ensure the buffer is null terminated in case this is string data)
		memset(bReadBuffer, 0, sizeof(bReadBuffer));
		if (ReadFile(hCommandOutputPipe, bReadBuffer, sizeof(bReadBuffer) - 1, &dwBytesRead, NULL) == 0)
		{
			// failed - check error code
			if (GetLastError() == ERROR_BROKEN_PIPE)
			{
				// pipe closed
				break;
			}
			else
			{
				// error
				CloseHandle(hCommandOutputPipe);
				return 1;
			}
		}

		// send current buffer to output function
		if (pCommandOutput(bReadBuffer, dwBytesRead, pCommandOutputParam) != 0)
		{
			// error
			CloseHandle(hCommandOutputPipe);
			return 1;
		}
	}

	// close handle
	CloseHandle(hCommandOutputPipe);

	return 0;
}

DWORD CommandOutput_StoreData(BYTE* pBufferData, DWORD dwBufferLength, BYTE* pParam)
{
	CommandOutput_StoreDataParamStruct* pCommandOutput_StoreDataParam = NULL;

	// get param
	pCommandOutput_StoreDataParam = (CommandOutput_StoreDataParamStruct*)pParam;

	// check if an output buffer was specified
	if (pCommandOutput_StoreDataParam->pOutputPtr != NULL)
	{
		// validate length
		if (dwBufferLength > (pCommandOutput_StoreDataParam->dwMaxOutputSize - pCommandOutput_StoreDataParam->dwTotalSize))
		{
			return 1;
		}

		// copy data
		memcpy((void*)(pCommandOutput_StoreDataParam->pOutputPtr + pCommandOutput_StoreDataParam->dwTotalSize), pBufferData, dwBufferLength);
	}

	// increase output size
	pCommandOutput_StoreDataParam->dwTotalSize += dwBufferLength;

	return 0;
}

// www.x86matthew.com
int main()
{
	BackgroundConsoleInstanceStruct BackgroundConsoleInstance;
	CommandOutput_StoreDataParamStruct CommandOutput_StoreDataParam;
	BYTE* pNtdllCopy = NULL;
	DWORD dwAllocSize = 0;
	const char* name = L"abc123\0";

	printf("Creating hidden cmd.exe process...\n");

	// create background console
	if (BackgroundConsole_Create(&name, &BackgroundConsoleInstance) != 0)
	{
		printf("error creaing console\n");
		return 1;
	}

	printf("Retrieving ntdll file size...\n");

	// call the function with a blank output buffer to retrieve the file size
	memset((void*)&CommandOutput_StoreDataParam, 0, sizeof(CommandOutput_StoreDataParam));
	CommandOutput_StoreDataParam.pOutputPtr = NULL;
	CommandOutput_StoreDataParam.dwMaxOutputSize = 0;
	CommandOutput_StoreDataParam.dwTotalSize = 0;
	if (BackgroundConsole_Exec(&BackgroundConsoleInstance, "type %windir%\\system32\\ntdll.dll\0", CommandOutput_StoreData, (BYTE*)&CommandOutput_StoreDataParam) != 0)
	{
		return 1;
	}

	printf("ntdll.dll file size: %u bytes - allocating memory...\n", CommandOutput_StoreDataParam.dwTotalSize);

	// allocate memory
	dwAllocSize = CommandOutput_StoreDataParam.dwTotalSize;
	pNtdllCopy = (BYTE*)malloc(dwAllocSize);
	if (pNtdllCopy == NULL)
	{
		return 1;
	}

	printf("Reading ntdll.dll data from disk...\n");

	// call the function again to read the file contents
	memset((void*)&CommandOutput_StoreDataParam, 0, sizeof(CommandOutput_StoreDataParam));
	CommandOutput_StoreDataParam.pOutputPtr = pNtdllCopy;
	CommandOutput_StoreDataParam.dwMaxOutputSize = dwAllocSize;
	CommandOutput_StoreDataParam.dwTotalSize = 0;
	if (BackgroundConsole_Exec(&BackgroundConsoleInstance, "type %windir%\\system32\\ntdll.dll", CommandOutput_StoreData, (BYTE*)&CommandOutput_StoreDataParam) != 0)
	{
		return 1;
	}

	printf("Read %u bytes successfully\n", CommandOutput_StoreDataParam.dwTotalSize);

	// (pNtdllCopy now contains a copy of ntdll)

	// clean up
	free(pNtdllCopy);
	BackgroundConsole_Close(&BackgroundConsoleInstance);

	return 0;
}
