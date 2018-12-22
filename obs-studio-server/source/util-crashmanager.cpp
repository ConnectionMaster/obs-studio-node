/*
* Modern effects for a modern Streamer
* Copyright (C) 2017 Michael Fabian Dirks
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#include "util-crashmanager.h"
#include <chrono>
#include <codecvt>
#include <iostream>
#include <locale>
#include <map>
#include <obs.h>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "nodeobs_api.h"

#if defined(_WIN32)

#include <WinBase.h>
#include "DbgHelp.h"
#include "Shlobj.h"
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "pdh.lib")
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <windows.h>
#include "TCHAR.h"
#include "pdh.h"
#include "psapi.h"

static PDH_HQUERY   cpuQuery;
static PDH_HCOUNTER cpuTotal;

#endif

// Global/static variables
util::CrashManager::CrashHandlerInfo* s_CrashHandlerInfo = nullptr;
std::vector<std::string>              s_HandledOBSCrashes;

// Forward
std::string    FormatVAString(const char* const format, va_list args);
nlohmann::json RewindCallStack(uint32_t skip, std::string& crashedMethod);
nlohmann::json RequestOBSLog();

// Class specific
util::CrashManager::~CrashManager()
{
	if (s_CrashHandlerInfo != nullptr)
		delete s_CrashHandlerInfo;
}

std::string PrettyBytes(uint64_t bytes)
{
	const char* suffixes[7];
	char        temp[100];
	suffixes[0]    = "b";
	suffixes[1]    = "kb";
	suffixes[2]    = "mb";
	suffixes[3]    = "gb";
	suffixes[4]    = "tb";
	suffixes[5]    = "pb";
	suffixes[6]    = "eb";
	uint64_t s     = 0; // which suffix to use
	double   count = bytes;
	while (count >= 1024 && s < 7) {
		s++;
		count /= 1024;
	}
	if (count - floor(count) == 0.0)
		sprintf(temp, "%d%s", (int)count, suffixes[s]);
	else
		sprintf(temp, "%.1f%s", count, suffixes[s]);

	return std::string(temp);
}

void RequestComputerUsageParams(
    long long& totalPhysMem,
    long long& physMemUsed,
    size_t&    physMemUsedByMe,
    double&    totalCPUUsed)
{
#if defined(_WIN32)

	MEMORYSTATUSEX          memInfo;
	PROCESS_MEMORY_COUNTERS pmc;
	PDH_FMT_COUNTERVALUE    counterVal;
	DWORDLONG               totalVirtualMem = memInfo.ullTotalPageFile;

	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	GlobalMemoryStatusEx(&memInfo);
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
	PdhCollectQueryData(cpuQuery);
	PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal);

	totalPhysMem    = memInfo.ullTotalPhys;
	physMemUsed     = (memInfo.ullTotalPhys - memInfo.ullAvailPhys);
	physMemUsedByMe = pmc.WorkingSetSize;
	totalCPUUsed    = counterVal.doubleValue;

#else

	// This link has info about the linux and Mac OS versions
	// https://stackoverflow.com/questions/63166/how-to-determine-cpu-and-memory-consumption-from-inside-a-process
	totalPhysMem    = long long(-1);
	physMemUsed     = long long(-1);
	physMemUsedByMe = size_t(-1);
	totalCPUUsed    = double(-1.0);

#endif
}

nlohmann::json RequestProcessList()
{
	DWORD          aProcesses[1024], cbNeeded, cProcesses;
	nlohmann::json result = nlohmann::json::object();

	if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded)) {
		return 1;
	}

	// Calculate how many process identifiers were returned
	cProcesses = cbNeeded / sizeof(DWORD);

	// Get the name and process identifier for each process
	for (unsigned i = 0; i < cProcesses; i++) {
		if (aProcesses[i] != 0) {
			TCHAR szProcessName[MAX_PATH] = TEXT("<unknown>");

			// Get a handle to the process
			DWORD  processID = aProcesses[i];
			HANDLE hProcess  = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processID);

			// Get the process name.
			if (NULL != hProcess) {
				HMODULE hMod;
				DWORD   cbNeeded;

				if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded)) {
					GetModuleBaseName(hProcess, hMod, szProcessName, sizeof(szProcessName) / sizeof(TCHAR));

					result.push_back(
					    {std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t>().to_bytes(szProcessName),
					     std::to_string(processID)});

					CloseHandle(hProcess);
				}
			}
		}
	}

	return result;
}

bool util::CrashManager::Initialize()
{
#ifndef _DEBUG

	s_CrashHandlerInfo = new CrashHandlerInfo();

	// Initialize sentry
	if (!SetupSentry())
		return false;

	// Handler for obs errors (mainly for bcrash() calls)
	base_set_crash_handler(
	    [](const char* format, va_list args, void* param) {
		    std::string errorMessage = FormatVAString(format, args);

		    // Check if this crash error is handled internally (if this is a known
		    // error that we can't do anything about it, just let the application
		    // crash normally
		    if (!TryHandleCrash(std::string(format), errorMessage))
			    HandleCrash(errorMessage);
	    },
	    nullptr);

	// Redirect all the calls from std::terminate
	std::set_terminate([]() { HandleCrash("Direct call to std::terminate"); });

#if defined(_WIN32)

	// Setup the windows exeption filter to
	SetUnhandledExceptionFilter([](struct _EXCEPTION_POINTERS* ExceptionInfo) {
		/* don't use if a debugger is present */
		if (IsDebuggerPresent())
			return LONG(EXCEPTION_CONTINUE_SEARCH);

		HandleCrash("UnhandledExceptionFilter");

		// Unreachable statement
		return LONG(EXCEPTION_CONTINUE_SEARCH);
	});

	// Setup the metrics query for the CPU usage
	// Ref: https://stackoverflow.com/questions/63166/how-to-determine-cpu-and-memory-consumption-from-inside-a-process
	PdhOpenQuery(NULL, NULL, &cpuQuery);
	PdhAddEnglishCounter(cpuQuery, L"\\Processor(_Total)\\% Processor Time", NULL, &cpuTotal);
	PdhCollectQueryData(cpuQuery);

#endif

	// The atexit will check if obs was safelly closed
	std::atexit(HandleExit);
	std::at_quick_exit(HandleExit);

#endif

	return true;
}

void util::CrashManager::Configure()
{
	// Add all obs crash messages that are supposed to be handled by our application and
	// shouldn't cause a crash report (because there is no point on reporting since we
	// cannot control them)
	// You don't need to set the entire message, we will just check for a substring match
	// in the main error message
	{
		s_HandledOBSCrashes.push_back("Failed to recreate D3D11");
		// ...
	}
}

bool util::CrashManager::SetupSentry()
{
#ifndef _DEBUG

	// This is the release dsn (the deprecated-but-in-use one, necessary for this lib)
	// Target https://sentry.io/streamlabs-obs/obs-server/
	s_CrashHandlerInfo->sentry = std::make_unique<nlohmann::crow>(
	    "https://ec98eac4e3ce49c7be1d83c8fb2005ef:1d6aec9118864fb4a2a6d7eda194ce45@sentry.io/1283431",
	    "https://sentry.io/api/1283431/minidump/?sentry_key=ec98eac4e3ce49c7be1d83c8fb2005ef",
	    nullptr,
	    1.0);

	// TODO: In the future, register here the mini-dump location (you need to set real filename that will exist if a crash
	// happens) and any other attachment file that should be sent with the crash report.
	// The quota here is 20mb for the entire message, including the JSON info that is always sent.
//  s_CrashHandlerInfo->sentry->register_minidump_upload("C:\\Users\\rodri\\12da1.dmp");
//  s_CrashHandlerInfo->sentry->register_file_upload("C:\\Users\\rodri\\Test.txt");

#endif

	return true;
}

void util::CrashManager::HandleExit() noexcept
{
	// If we are exiting normally and obs is active, we have a problem because some
	// modules and threads could potentially be running and this will result in a
	// crash that is masked. The real issue here is why we are exiting without
	// finishing the obs first.
	if (obs_initialized()) {
		// Proceed to add more info to our crash reporter but don't call abort, we
		// cannot ensure that when at exit a call to obs_initialized will be safe, it
		// could be in an invalid state, we will let the application continue and if
		// this results in a crash at least we will know what caused it
		HandleCrash("AtExit", false);
	}
}

void util::CrashManager::HandleCrash(std::string _crashInfo, bool _callAbort) noexcept
{
	// If for any reason this is true, it means that we are crashing inside this same
	// method, if that happens just call abort and ignore any remaining processing since
	// we cannot continue.
	static bool insideCrashMethod = false;
	if (insideCrashMethod)
		abort();

	insideCrashMethod = true;

	// This will manually rewind the callstack, we will use this info to populate an
	// cras report attribute, avoiding some cases that the memory dump is corrupted
	// and we don't have access to the callstack.
	std::string    crashedMethodName;
	nlohmann::json callStack = RewindCallStack(0, crashedMethodName);

	// Get the information about the total of CPU and RAM used by this user
	long long totalPhysMem;
	long long physMemUsed;
	double    totalCPUUsed;
	size_t    physMemUsedByMe;
	RequestComputerUsageParams(totalPhysMem, physMemUsed, physMemUsedByMe, totalCPUUsed);

	// Setup all the custom annotations that are important too our crash report
	auto& sentry = s_CrashHandlerInfo->sentry;
	sentry->add_tags_context({{"status", obs_initialized() ? "initialized" : "shutdown"}});
	sentry->add_tags_context({{"leaks", std::to_string(bnum_allocs())}});
	sentry->add_tags_context({{"total memory", PrettyBytes(totalPhysMem)}});
	sentry->add_tags_context({{"total used memory", PrettyBytes(physMemUsed)}});
	sentry->add_tags_context({{"total SLOBS memory", PrettyBytes(physMemUsedByMe)}});
	sentry->add_tags_context({{"cpu", std::to_string(int(totalCPUUsed)) + "%"}});

	// Add obs logs
	sentry->add_extra_context({{"OBS Log", RequestOBSLog()}});
	sentry->add_extra_context({{"Process List", RequestProcessList()}});

	// Invoke the crash report
	InvokeReport(_crashInfo, crashedMethodName, callStack);

	// Finish with a call to abort since there is no point on continuing
	if (_callAbort)
		abort();

	// Unreachable statement if _callAbort is true
	insideCrashMethod = false;
}

bool util::CrashManager::TryHandleCrash(std::string _format, std::string _crashMessage)
{
	bool crashIsHandled = false;
	for (auto& handledCrashes : s_HandledOBSCrashes) {
		if (std::string(_format).find(handledCrashes) != std::string::npos) {
			crashIsHandled = true;
			break;
		}
	}

	if (!crashIsHandled)
		return false;

	// If we are here this is a known crash that we don't wanna propagate to the crashpad
	// (because we don't need to handle it or we cannot control it), the ideal would be just
	// a call to stop the crashpad handler but since it doesn't have a method like this we
	// can try to finish the application normally to avoid any crash report

	// Optionally send a message to the user
	if (false) {
		std::string errorMessage = std::string(
		    "The Streamlabs OBS encontered an internal error and will now close, if you are receiving this "
		    "type of error frequently please contact our support! Error message: "
		    + _crashMessage);

		MessageBox(
		    nullptr,
		    std::wstring(errorMessage.begin(), errorMessage.end()).c_str(),
		    TEXT("Streamlabs OBS Crash"),
		    MB_OK);
	}

	// If we cannot destroy the obs and exit normally without causing a crash report,
	// proceed with a crash
	try {
		// If for any reason a call to bcrash is made when inside the shutdown method below, the
		// obs will internally detect this and call exit(2), that will call our method HandleExit
		// that will handle this crash, because that there is no point on checking for recursive
		// calls
		OBS_API::destroyOBS_API();
		exit(0);
	} catch (...) {
		util::CrashManager::HandleCrash(_crashMessage);
	}

	// Unreachable statement
	return true;
}

// Format a var arg string into a c++ std::string type
std::string FormatVAString(const char* const format, va_list args)
{
	auto temp   = std::vector<char>{};
	auto length = std::size_t{63};
	while (temp.size() <= length) {
		temp.resize(length + 1);
		const auto status = std::vsnprintf(temp.data(), temp.size(), format, args);
		if (status < 0)
			throw std::runtime_error{"string formatting error"};
		length = static_cast<std::size_t>(status);
	}
	return std::string{temp.data(), length};
}

nlohmann::json RewindCallStack(uint32_t skip, std::string& crashedMethod)
{
	nlohmann::json result = json::array();

#ifndef _DEBUG
#if defined(_WIN32)

	// Get the function to rewing the callstack
	typedef USHORT(WINAPI * CaptureStackBackTraceType)(__in ULONG, __in ULONG, __out PVOID*, __out_opt PULONG);
	CaptureStackBackTraceType func =
	    (CaptureStackBackTraceType)(GetProcAddress(LoadLibrary(L"kernel32.dll"), "RtlCaptureStackBackTrace"));
	if (func == NULL)
		return result; // WOE 29.SEP.2010

	// Quote from Microsoft Documentation:
	// ## Windows Server 2003 and Windows XP:
	// ## The sum of the FramesToSkip and FramesToCapture parameters must be less than 63.
	const int       kMaxCallers = 62;
	void*           callers_stack[kMaxCallers];
	unsigned short  frames;
	SYMBOL_INFO*    symbol;
	HANDLE          process;
	DWORD           dwDisplacement = 0;
	IMAGEHLP_LINE64 line;

	// Get the callstack information
	SymSetOptions(SYMOPT_LOAD_LINES);
	process = GetCurrentProcess();
	SymInitialize(process, NULL, TRUE);
	frames               = (func)(0, kMaxCallers, callers_stack, NULL);
	symbol               = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
	symbol->MaxNameLen   = 255;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	std::vector<std::string> callstack;
	int                      writingIndex = -1;

	// Currently 18 is the maximum that we can display on backtrace in one single attribute
	const unsigned short MAX_CALLERS_SHOWN = 50;
	frames                                 = frames < MAX_CALLERS_SHOWN ? frames : MAX_CALLERS_SHOWN;
	std::vector<int> missingFrames;
	for (int i = frames - 1; i >= int(skip); i--) {
		if (!SymFromAddr(process, (DWORD64)(callers_stack[i]), 0, symbol)
		    || !SymGetLineFromAddr64(process, (DWORD64)(callers_stack[i]), &dwDisplacement, &line)) {
			// Add the frame index to the missing frames vector
			missingFrames.push_back(i);
			continue;
		}

		std::stringstream buffer;
		std::string       fullPath = line.FileName;
		std::string       fileName;
		std::string       functionName = symbol->Name;
		std::string       instructionAddress;
		std::string       symbolAddress = std::to_string(symbol->Address);

		// Get the filename from the fullpath
		size_t pos = fullPath.rfind("\\", fullPath.length());
		if (pos != string::npos) {
			fileName = (fullPath.substr(pos + 1, fullPath.length() - pos));
		}

		// Ignore any report that refers to this file
		if (fileName == "util-crashmanager.cpp")
			continue;

		// Store the callstack address into the buffer (void* to std::string)
		buffer << callers_stack[i];

		// A little of magic to shorten the address lenght
		instructionAddress = buffer.str();
		pos                = instructionAddress.find_first_not_of("0");
		if (pos != string::npos && pos <= 4) {
			instructionAddress = (instructionAddress.substr(pos + 1, instructionAddress.length() - pos));
		}

		// Transform the addresses to lowercase
		transform(instructionAddress.begin(), instructionAddress.end(), instructionAddress.begin(), ::tolower);
		transform(symbolAddress.begin(), symbolAddress.end(), symbolAddress.begin(), ::tolower);

		json entry;
		entry["filename"]         = functionName; // The swap with the function name is intentional
		entry["function"]         = fileName;
		entry["lineno"]           = line.LineNumber;
		entry["instruction_addr"] = "0x" + instructionAddress;
		entry["symbol_addr"]      = "0x" + symbolAddress;

		if (functionName.substr(0, 5) == "std::" || functionName.substr(0, 2) == "__") {
			entry["in_app"] = false;
		}

		// If we have some missing frames
		if (missingFrames.size() > 0) {
			entry["frames_omitted"] = {std::to_string(missingFrames.back()), std::to_string(i)};
			missingFrames.clear();
		}

		// Set the crashed method name
		crashedMethod = functionName;

		result.push_back(entry);
	}

	free(symbol);

#endif
#endif

	return result;
}

nlohmann::json RequestOBSLog()
{
	nlohmann::json result;

	// Setup the obs log queue as an attribute
	if (OBS_API::getOBSLogQueue().size() > 0) {
		std::queue<std::string> obsLogQueue = OBS_API::getOBSLogQueue();
		std::string             obsLogMessage;
		while (obsLogQueue.size() > 0) {
			result.push_back(obsLogQueue.front());
			obsLogQueue.pop();
		}
	}

	return result;
}

void util::CrashManager::InvokeReport(std::string _crashInfo, std::string _complementInfo, nlohmann::json _callStack)
{
	if (s_CrashHandlerInfo == nullptr)
		return;

#ifndef _DEBUG

	// Capture the message and wait, this wait is necessary since we don't have a way to force it to send
	// the message and wait for it's completion at the same time
	s_CrashHandlerInfo->sentry->capture_exception_sync(_crashInfo, _complementInfo, "", _callStack, nullptr, false);

#endif
}

void BindCrtHandlesToStdHandles(bool bindStdIn, bool bindStdOut, bool bindStdErr)
{
	// Re-initialize the C runtime "FILE" handles with clean handles bound to "nul". We do this because it has been
	// observed that the file number of our standard handle file objects can be assigned internally to a value of -2
	// when not bound to a valid target, which represents some kind of unknown internal invalid state. In this state our
	// call to "_dup2" fails, as it specifically tests to ensure that the target file number isn't equal to this value
	// before allowing the operation to continue. We can resolve this issue by first "re-opening" the target files to
	// use the "nul" device, which will place them into a valid state, after which we can redirect them to our target
	// using the "_dup2" function.
	if (bindStdIn) {
		FILE* dummyFile;
		freopen_s(&dummyFile, "nul", "r", stdin);
	}
	if (bindStdOut) {
		FILE* dummyFile;
		freopen_s(&dummyFile, "nul", "w", stdout);
	}
	if (bindStdErr) {
		FILE* dummyFile;
		freopen_s(&dummyFile, "nul", "w", stderr);
	}

	// Redirect unbuffered stdin from the current standard input handle
	if (bindStdIn) {
		HANDLE stdHandle = GetStdHandle(STD_INPUT_HANDLE);
		if (stdHandle != INVALID_HANDLE_VALUE) {
			int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
			if (fileDescriptor != -1) {
				FILE* file = _fdopen(fileDescriptor, "r");
				if (file != NULL) {
					int dup2Result = _dup2(_fileno(file), _fileno(stdin));
					if (dup2Result == 0) {
						setvbuf(stdin, NULL, _IONBF, 0);
					}
				}
			}
		}
	}

	// Redirect unbuffered stdout to the current standard output handle
	if (bindStdOut) {
		HANDLE stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
		if (stdHandle != INVALID_HANDLE_VALUE) {
			int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
			if (fileDescriptor != -1) {
				FILE* file = _fdopen(fileDescriptor, "w");
				if (file != NULL) {
					int dup2Result = _dup2(_fileno(file), _fileno(stdout));
					if (dup2Result == 0) {
						setvbuf(stdout, NULL, _IONBF, 0);
					}
				}
			}
		}
	}

	// Redirect unbuffered stderr to the current standard error handle
	if (bindStdErr) {
		HANDLE stdHandle = GetStdHandle(STD_ERROR_HANDLE);
		if (stdHandle != INVALID_HANDLE_VALUE) {
			int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
			if (fileDescriptor != -1) {
				FILE* file = _fdopen(fileDescriptor, "w");
				if (file != NULL) {
					int dup2Result = _dup2(_fileno(file), _fileno(stderr));
					if (dup2Result == 0) {
						setvbuf(stderr, NULL, _IONBF, 0);
					}
				}
			}
		}
	}

	// Clear the error state for each of the C++ standard stream objects. We need to do this, as attempts to access the
	// standard streams before they refer to a valid target will cause the iostream objects to enter an error state. In
	// versions of Visual Studio after 2005, this seems to always occur during startup regardless of whether anything
	// has been read from or written to the targets or not.
	if (bindStdIn) {
		std::wcin.clear();
		std::cin.clear();
	}
	if (bindStdOut) {
		std::wcout.clear();
		std::cout.clear();
	}
	if (bindStdErr) {
		std::wcerr.clear();
		std::cerr.clear();
	}
}

void util::CrashManager::OpenConsole()
{
#if defined(_WIN32)

	// Allocate a console window for this process
	AllocConsole();

	// Update the C/C++ runtime standard input, output, and error targets to use the console window
	BindCrtHandlesToStdHandles(true, true, true);

#endif
}

void util::CrashManager::IPCValuesToData(const std::vector<ipc::value>& values, nlohmann::json& data)
{
	int paramCounter = 0;
	for (auto& value : values) {
		switch (value.type) {
		case ipc::type::Null: {
			data.push_back({"arg" + std::to_string(paramCounter), "null"});
			break;
		}
		case ipc::type::Float: {
			data.push_back({"arg" + std::to_string(paramCounter), std::to_string(value.value_union.fp32)});
			break;
		}
		case ipc::type::Double: {
			data.push_back({"arg" + std::to_string(paramCounter), std::to_string(value.value_union.fp64)});
			break;
		}
		case ipc::type::Int32: {
			data.push_back({"arg" + std::to_string(paramCounter), std::to_string(value.value_union.i32)});
			break;
		}
		case ipc::type::Int64: {
			data.push_back({"arg" + std::to_string(paramCounter), std::to_string(value.value_union.i64)});
			break;
		}
		case ipc::type::UInt32: {
			data.push_back({"arg" + std::to_string(paramCounter), std::to_string(value.value_union.ui32)});
			break;
		}
		case ipc::type::UInt64: {
			data.push_back({"arg" + std::to_string(paramCounter), std::to_string(value.value_union.ui64)});
			break;
		}
		case ipc::type::String: {
			data.push_back({"arg" + std::to_string(paramCounter), value.value_str});
			break;
		}
		case ipc::type::Binary: {
			data.push_back({"arg" + std::to_string(paramCounter), ""});
			break;
		}
		}

		paramCounter++;
	}
}

void util::CrashManager::AddBreadcrumb(const std::string& message, const nlohmann::json& attributes)
{
	if (s_CrashHandlerInfo == nullptr)
		return;

	s_CrashHandlerInfo->sentry->add_breadcrumb(message, attributes);
}

void util::CrashManager::ClearBreadcrumbs()
{
	if (s_CrashHandlerInfo == nullptr)
		return;

	s_CrashHandlerInfo->sentry->clear_context();
}