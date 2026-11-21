// CustomAction.cpp : Defines the entry point for the custom action.
#include "pch.h"
#include <stdlib.h>
#include <string.h>
#include <strutil.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <netfw.h>
#include <shlwapi.h>

#include "./Common.h"

#pragma comment(lib, "Shlwapi.lib")

UINT __stdcall CustomActionHello(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    hr = WcaInitialize(hInstall, "CustomActionHello");
    ExitOnFailure(hr, "Failed to initialize");

    WcaLog(LOGMSG_STANDARD, "Initialized.");

    // TODO: Add your custom action code here.
    WcaLog(LOGMSG_STANDARD, "================= Example CustomAction Hello");

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

// Helper function to safely delete a file using handle-based deletion.
// Directories are refused after opening the handle.
BOOL SafeDeleteItem(LPCWSTR fullPath)
{
    // Open the file/directory with delete and attribute-read access plus FILE_FLAG_OPEN_REPARSE_POINT
    // to prevent following symlinks.
    // Use shared access to allow deletion even when other processes have the file open.
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE hFile = CreateFileW(
        fullPath,
        DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,  // Allow shared access
        NULL,
        OPEN_EXISTING,
        flags,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        WcaLog(LOGMSG_STANDARD, "SafeDeleteItem: Failed to open '%ls'. Error: %lu", fullPath, GetLastError());
        return FALSE;
    }

    BY_HANDLE_FILE_INFORMATION fileInfo;
    if (FALSE == GetFileInformationByHandle(hFile, &fileInfo))
    {
        WcaLog(LOGMSG_STANDARD, "SafeDeleteItem: Failed to inspect '%ls'. Error: %lu", fullPath, GetLastError());
        CloseHandle(hFile);
        return FALSE;
    }

    if (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        WcaLog(LOGMSG_STANDARD, "SafeDeleteItem: Refusing to delete directory '%ls'.", fullPath);
        CloseHandle(hFile);
        return FALSE;
    }

    // Use SetFileInformationByHandle to mark for deletion.
    // The file will be deleted when the handle is closed.
    FILE_DISPOSITION_INFO dispInfo;
    dispInfo.DeleteFile = TRUE;

    BOOL result = SetFileInformationByHandle(
        hFile,
        FileDispositionInfo,
        &dispInfo,
        sizeof(dispInfo)
    );

    if (!result)
    {
        DWORD error = GetLastError();
        WcaLog(LOGMSG_STANDARD, "SafeDeleteItem: Failed to mark '%ls' for deletion. Error: %lu", fullPath, error);
    }

    CloseHandle(hFile);
    return result;
}

BOOL PathEndsWithSlash(LPCWSTR path)
{
    size_t length = 0;
    HRESULT hr = StringCchLengthW(path, MAX_PATH, &length);
    if (FAILED(hr) || length == 0)
    {
        return FALSE;
    }

    WCHAR last = path[length - 1];
    return last == L'\\' || last == L'/';
}

void ClearReadOnlyAttribute(LPCWSTR fullPath, DWORD attributes)
{
    if (!(attributes & FILE_ATTRIBUTE_READONLY))
    {
        return;
    }

    DWORD writableAttributes = attributes & ~FILE_ATTRIBUTE_READONLY;
    if (writableAttributes == 0)
    {
        writableAttributes = FILE_ATTRIBUTE_NORMAL;
    }

    if (SetFileAttributesW(fullPath, writableAttributes))
    {
        WcaLog(LOGMSG_STANDARD, "Runtime cleanup cleared read-only attribute for '%ls'.", fullPath);
        return;
    }

    WcaLog(LOGMSG_STANDARD, "Runtime cleanup failed to clear read-only attribute for '%ls'. Error: %lu", fullPath, GetLastError());
}

BOOL DeleteRuntimeGeneratedFile(LPCWSTR installFolder, LPCWSTR fileName)
{
    WCHAR fullPath[MAX_PATH];
    LPCWSTR separator = PathEndsWithSlash(installFolder) ? L"" : L"\\";
    HRESULT hr = StringCchPrintfW(fullPath, MAX_PATH, L"%s%s%s", installFolder, separator, fileName);
    if (FAILED(hr))
    {
        WcaLog(LOGMSG_STANDARD, "Runtime cleanup path is too long for '%ls'.", fileName);
        return FALSE;
    }

    DWORD attributes = GetFileAttributesW(fullPath);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            return TRUE;
        }

        WcaLog(LOGMSG_STANDARD, "Runtime cleanup cannot stat '%ls'. Error: %lu", fullPath, error);
        return FALSE;
    }

    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        WcaLog(LOGMSG_STANDARD, "Runtime cleanup skipped directory '%ls'.", fullPath);
        return FALSE;
    }

    ClearReadOnlyAttribute(fullPath, attributes);
    WcaLog(LOGMSG_STANDARD, "Runtime cleanup deleting '%ls'.", fullPath);
    return SafeDeleteItem(fullPath);
}

// See `Package.wxs` for the sequence of this custom action.
//
// Upgrade/uninstall sequence:
//   1. InstallInitialize
//   2. RemoveExistingProducts
//      ├─ TerminateProcesses
//      ├─ TryStopDeleteService
//      ├─ RemoveRuntimeGeneratedFiles - <-- Here
//      └─ RemoveFiles
//   3. InstallValidate
//   4. InstallFiles
//   5. InstallExecute
//   6. InstallFinalize
UINT __stdcall RemoveRuntimeGeneratedFiles(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    LPWSTR installFolder = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;

    hr = WcaInitialize(hInstall, "RemoveRuntimeGeneratedFiles");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &installFolder);
    ExitOnFailure(hr, "failed to read install folder from custom action data: %ls", pwz);

    if (installFolder == NULL || installFolder[0] == L'\0') {
        WcaLog(LOGMSG_STANDARD, "Install folder path is empty, skipping runtime cleanup.");
        goto LExit;
    }

    if (PathIsRootW(installFolder)) {
        WcaLog(LOGMSG_STANDARD, "Refusing runtime cleanup in root folder '%ls'.", installFolder);
        goto LExit;
    }

    WcaLog(LOGMSG_STANDARD, "Removing runtime-generated files from install folder: %ls", installFolder);
    DeleteRuntimeGeneratedFile(installFolder, L"RuntimeBroker_rustdesk.exe");

LExit:
    ReleaseStr(pwzData);

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

// https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryinformationprocess
// **NtQueryInformationProcess** may be altered or unavailable in future versions of Windows.
// Applications should use the alternate functions listed in this topic.
// But I do not find the alternate functions.
// https://github.com/heim-rs/heim/issues/105#issuecomment-683647573
typedef NTSTATUS(NTAPI *pfnNtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
bool TerminateProcessIfNotContainsParam(pfnNtQueryInformationProcess NtQueryInformationProcess, HANDLE process, LPCWSTR excludeParam)
{
    bool processClosed = false;
    PROCESS_BASIC_INFORMATION processInfo;
    NTSTATUS status = NtQueryInformationProcess(process, ProcessBasicInformation, &processInfo, sizeof(processInfo), NULL);
    if (status == 0 && processInfo.PebBaseAddress != NULL)
    {
        PEB peb;
        SIZE_T dwBytesRead;
        if (ReadProcessMemory(process, processInfo.PebBaseAddress, &peb, sizeof(peb), &dwBytesRead))
        {
            RTL_USER_PROCESS_PARAMETERS pebUpp;
            if (ReadProcessMemory(process,
                                  peb.ProcessParameters,
                                  &pebUpp,
                                  sizeof(RTL_USER_PROCESS_PARAMETERS),
                                  &dwBytesRead))
            {
                if (pebUpp.CommandLine.Length > 0)
                {
                    // Allocate extra space for null terminator
                    WCHAR *commandLine = (WCHAR *)malloc(pebUpp.CommandLine.Length + sizeof(WCHAR));
                    if (commandLine != NULL)
                    {
                        // Initialize all bytes to zero for safety
                        memset(commandLine, 0, pebUpp.CommandLine.Length + sizeof(WCHAR));
                        if (ReadProcessMemory(process, pebUpp.CommandLine.Buffer,
                                              commandLine, pebUpp.CommandLine.Length, &dwBytesRead))
                        {
                            if (wcsstr(commandLine, excludeParam) == NULL)
                            {
                                WcaLog(LOGMSG_STANDARD, "Terminate process : %ls", commandLine);
                                TerminateProcess(process, 0);
                                processClosed = true;
                            }
                        }
                        free(commandLine);
                    }
                }
            }
        }
    }
    return processClosed;
}

// Terminate processes that do not have parameter [excludeParam]
// Note. This function relies on "NtQueryInformationProcess",
//       which may not be found.
//       Then all processes of [processName] will be terminated.
bool TerminateProcessesByNameW(LPCWSTR processName, LPCWSTR excludeParam)
{
    HMODULE hntdll = GetModuleHandleW(L"ntdll.dll");
    if (hntdll == NULL)
    {
        WcaLog(LOGMSG_STANDARD, "Failed to load ntdll.");
    }

    pfnNtQueryInformationProcess NtQueryInformationProcess = NULL;
    if (hntdll != NULL)
    {
        NtQueryInformationProcess = (pfnNtQueryInformationProcess)GetProcAddress(
            hntdll, "NtQueryInformationProcess");
    }
    if (NtQueryInformationProcess == NULL)
    {
        WcaLog(LOGMSG_STANDARD, "Failed to get address of NtQueryInformationProcess.");
    }

    bool processClosed = false;
    // Create a snapshot of the current system processes
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(snapshot, &processEntry))
        {
            do
            {
                if (lstrcmpiW(processName, processEntry.szExeFile) == 0)
                {
                    HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processEntry.th32ProcessID);
                    if (process != NULL)
                    {
                        if (NtQueryInformationProcess == NULL)
                        {
                            WcaLog(LOGMSG_STANDARD, "Terminate process : %ls, while NtQueryInformationProcess is NULL", processName);
                            TerminateProcess(process, 0);
                            processClosed = true;
                        }
                        else
                        {
                            processClosed = TerminateProcessIfNotContainsParam(
                                NtQueryInformationProcess,
                                process,
                                excludeParam);
                        }
                        CloseHandle(process);
                    }
                }
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot);
    }
    if (hntdll != NULL)
    {
        CloseHandle(hntdll);
    }
    return processClosed;
}

UINT __stdcall TerminateProcesses(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    wchar_t szProcess[256] = {0};
    DWORD cchProcess = sizeof(szProcess) / sizeof(szProcess[0]);

    hr = WcaInitialize(hInstall, "TerminateProcesses");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"TerminateProcesses", szProcess, &cchProcess);

    WcaLog(LOGMSG_STANDARD, "Try terminate processes : %ls", szProcess);
    TerminateProcessesByNameW(szProcess, L"--install");

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

// No use for now, it can be refer as an example of ShellExecuteW.
void AddFirewallRuleCmdline(LPWSTR exeName, LPWSTR exeFile, LPCWSTR dir)
{
    HRESULT hr = S_OK;
    HINSTANCE hi = 0;
    WCHAR cmdline[1024] = { 0, };
    WCHAR rulename[500] = { 0, };

    StringCchPrintfW(rulename, sizeof(rulename) / sizeof(rulename[0]), L"%ls Service", exeName);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make rulename: %ls", exeName);
        return;
    }

    StringCchPrintfW(cmdline, sizeof(cmdline) / sizeof(cmdline[0]), L"advfirewall firewall add rule name=\"%ls\" dir=%ls action=allow program=\"%ls\" enable=yes", rulename, dir, exeFile);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make cmdline: %ls", exeName);
        return;
    }

    hi = ShellExecuteW(NULL, L"open", L"netsh", cmdline, NULL, SW_HIDE);
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to change firewall rule : %d, last error: %d", (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Firewall rule \"%ls\" (%ls) is added", rulename, dir);
    }
}

// No use for now, it can be refer as an example of ShellExecuteW.
void RemoveFirewallRuleCmdline(LPWSTR exeName)
{
    HRESULT hr = S_OK;
    HINSTANCE hi = 0;
    WCHAR cmdline[1024] = { 0, };
    WCHAR rulename[500] = { 0, };

    StringCchPrintfW(rulename, sizeof(rulename) / sizeof(rulename[0]), L"%ls Service", exeName);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make rulename: %ls", exeName);
        return;
    }

    StringCchPrintfW(cmdline, sizeof(cmdline) / sizeof(cmdline[0]), L"advfirewall firewall delete rule name=\"%ls\"", rulename);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make cmdline: %ls", exeName);
        return;
    }

    hi = ShellExecuteW(NULL, L"open", L"netsh", cmdline, NULL, SW_HIDE);
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to change firewall rule \"%ls\" : %d, last error: %d", rulename, (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Firewall rule \"%ls\" is removed", rulename);
    }
}

UINT __stdcall AddFirewallRules(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR exeFile = NULL;
    LPWSTR exeName = NULL;
    WCHAR exeNameNoExt[500] = { 0, };
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;
    size_t szNameLen = 0;

    hr = WcaInitialize(hInstall, "AddFirewallRules");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &exeFile);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);
    WcaLog(LOGMSG_STANDARD, "Try add firewall exceptions for file : %ls", exeFile);

    exeName = PathFindFileNameW(exeFile + 1);
    hr = StringCchPrintfW(exeNameNoExt, 500, exeName);
    ExitOnFailure(hr, "Failed to copy exe name: %ls", exeName);
    szNameLen = wcslen(exeNameNoExt);
    if (szNameLen >= 4 && wcscmp(exeNameNoExt + szNameLen - 4, L".exe") == 0) {
        exeNameNoExt[szNameLen - 4] = L'\0';
    }

    //if (exeFile[0] == L'1') {
    //    AddFirewallRuleCmdline(exeNameNoExt, exeFile, L"in");
    //    AddFirewallRuleCmdline(exeNameNoExt, exeFile, L"out");
    //}
    //else {
    //    RemoveFirewallRuleCmdline(exeNameNoExt);
    //}

    AddFirewallRule(exeFile[0] == L'1', exeNameNoExt, exeFile + 1);

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall SetPropertyIsServiceRunning(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    wchar_t szAppName[500] = { 0 };
    DWORD cchAppName = sizeof(szAppName) / sizeof(szAppName[0]);
    wchar_t szPropertyName[500] = { 0 };
    DWORD cchPropertyName = sizeof(szPropertyName) / sizeof(szPropertyName[0]);
    bool isRunning = false;

    hr = WcaInitialize(hInstall, "SetPropertyIsServiceRunning");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"AppName", szAppName, &cchAppName);
    WcaLog(LOGMSG_STANDARD, "Try query service of : \"%ls\"", szAppName);

    MsiGetPropertyW(hInstall, L"PropertyName", szPropertyName, &cchPropertyName);
    WcaLog(LOGMSG_STANDARD, "Try set is service running, property name : \"%ls\"", szPropertyName);

    isRunning = IsServiceRunningW(szAppName);
    MsiSetPropertyW(hInstall, szPropertyName, isRunning ? L"'N'" : L"'Y'");

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

void TryCreateStartServiceByShell(LPWSTR svcName, LPWSTR svcBinary, LPWSTR szSvcDisplayName);
UINT __stdcall CreateStartService(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    LPWSTR svcParams = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;
    LPWSTR svcName = NULL;
    LPWSTR svcBinary = NULL;
    wchar_t szSvcDisplayName[500] = { 0 };
    DWORD cchSvcDisplayName = sizeof(szSvcDisplayName) / sizeof(szSvcDisplayName[0]);

    hr = WcaInitialize(hInstall, "CreateStartService");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &svcParams);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);

    WcaLog(LOGMSG_STANDARD, "Try create start service : %ls", svcParams);

    svcName = svcParams;
    svcBinary = wcschr(svcParams, L';');
    if (svcBinary == NULL) {
        WcaLog(LOGMSG_STANDARD, "Failed to find binary : %ls", svcParams);
        goto LExit;
    }
    svcBinary[0] = L'\0';
    svcBinary += 1;

    hr = StringCchPrintfW(szSvcDisplayName, cchSvcDisplayName, L"%ls Service", svcName);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");
    if (MyCreateServiceW(svcName, szSvcDisplayName, svcBinary)) {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is created.", svcName);
        if (MyStartServiceW(svcName)) {
            WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is started.", svcName);
        }
        else {
            WcaLog(LOGMSG_STANDARD, "Failed to start service: \"%ls\"", svcName);
        }
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Failed to create service: \"%ls\"", svcName);
    }

    if (IsServiceRunningW(svcName)) {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is running.", svcName);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is not running, try create and start service by shell", svcName);
        TryCreateStartServiceByShell(svcName, svcBinary, szSvcDisplayName);
    }

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

void TryStopDeleteServiceByShell(LPWSTR svcName);
UINT __stdcall TryStopDeleteService(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR svcName = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;
    wchar_t szExeFile[500] = { 0 };
    DWORD cchExeFile = sizeof(szExeFile) / sizeof(szExeFile[0]);
    SERVICE_STATUS_PROCESS svcStatus;
    DWORD lastErrorCode = 0;

    hr = WcaInitialize(hInstall, "TryStopDeleteService");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &svcName);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);
    WcaLog(LOGMSG_STANDARD, "Try stop and delete service : %ls", svcName);

    if (MyStopServiceW(svcName)) {
        for (int i = 0; i < 10; i++) {
            if (IsServiceRunningW(svcName)) {
                Sleep(100);
            }
            else {
                break;
            }
        }
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Failed to stop service: \"%ls\", error: 0x%02X.", svcName, GetLastError());
    }

    if (IsServiceRunningW(svcName)) {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is not stopped after 1000 ms.", svcName);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is stopped.", svcName);
    }

    if (MyDeleteServiceW(svcName)) {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" deletion is completed without errors.", svcName);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Failed to delete service: \"%ls\", error: 0x%02X.", svcName, GetLastError());
    }

    if (QueryServiceStatusExW(svcName, &svcStatus)) {
        WcaLog(LOGMSG_STANDARD, "Failed to delete service: \"%ls\", current status: %d.", svcName, svcStatus.dwCurrentState);
        TryStopDeleteServiceByShell(svcName);
    }
    else {
        lastErrorCode = GetLastError();
        if (lastErrorCode == ERROR_SERVICE_DOES_NOT_EXIST) {
            WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is deleted.", svcName);
        }
        else {
            WcaLog(LOGMSG_STANDARD, "Failed to query service status: \"%ls\", error: 0x%02X.", svcName, lastErrorCode);
            TryStopDeleteServiceByShell(svcName);
        }
    }

    // It's really strange that we need sleep here.
    // But the upgrading may be stuck at "copying new files" because the file is in using.
    // Steps to reproduce: Install -> stop service in tray --> start service -> upgrade
    // Sleep(300);

    // Or we can terminate the process
    hr = StringCchPrintfW(szExeFile, cchExeFile, L"%ls.exe", svcName);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");
    TerminateProcessesByNameW(szExeFile, L"--not-in-use");

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall TryDeleteStartupShortcut(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    wchar_t szShortcut[500] = { 0 };
    DWORD cchShortcut = sizeof(szShortcut) / sizeof(szShortcut[0]);
    wchar_t szStartupDir[500] = { 0 };
    DWORD cchStartupDir = sizeof(szStartupDir) / sizeof(szStartupDir[0]);
    WCHAR pwszTemp[1024] = L"";

    hr = WcaInitialize(hInstall, "DeleteStartupShortcut");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"StartupFolder", szStartupDir, &cchStartupDir);

    MsiGetPropertyW(hInstall, L"ShortcutName", szShortcut, &cchShortcut);
    WcaLog(LOGMSG_STANDARD, "Try delete startup shortcut of : \"%ls\"", szShortcut);

    hr = StringCchPrintfW(pwszTemp, 1024, L"%ls%ls.lnk", szStartupDir, szShortcut);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");

    if (DeleteFileW(pwszTemp)) {
        WcaLog(LOGMSG_STANDARD, "Failed to delete startup shortcut of : \"%ls\"", pwszTemp);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Startup shortcut is deleted : \"%ls\"", pwszTemp);
    }

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall SetPropertyFromConfig(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    wchar_t szConfigFile[1024] = { 0 };
    DWORD cchConfigFile = sizeof(szConfigFile) / sizeof(szConfigFile[0]);
    wchar_t szConfigKey[500] = { 0 };
    DWORD cchConfigKey = sizeof(szConfigKey) / sizeof(szConfigKey[0]);
    wchar_t szPropertyName[500] = { 0 };
    DWORD cchPropertyName = sizeof(szPropertyName) / sizeof(szPropertyName[0]);
    std::wstring configValue;

    hr = WcaInitialize(hInstall, "SetPropertyFromConfig");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"ConfigFile", szConfigFile, &cchConfigFile);
    WcaLog(LOGMSG_STANDARD, "Try read config file of : \"%ls\"", szConfigFile);

    MsiGetPropertyW(hInstall, L"ConfigKey", szConfigKey, &cchConfigKey);
    WcaLog(LOGMSG_STANDARD, "Try read configuration, config key : \"%ls\"", szConfigKey);

    MsiGetPropertyW(hInstall, L"PropertyName", szPropertyName, &cchPropertyName);
    WcaLog(LOGMSG_STANDARD, "Try read configuration, property name : \"%ls\"", szPropertyName);

    configValue = ReadConfig(szConfigFile, szConfigKey);
    MsiSetPropertyW(hInstall, szPropertyName, configValue.c_str());

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall AddRegSoftwareSASGeneration(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

     LSTATUS result = 0;
     HKEY hKey;
     LPCWSTR subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
     LPCWSTR valueName = L"SoftwareSASGeneration";
     DWORD valueType = REG_DWORD;
     DWORD valueData = 1;
     DWORD valueDataSize = sizeof(DWORD);

    HINSTANCE hi = 0;

    hr = WcaInitialize(hInstall, "AddRegSoftwareSASGeneration");
    ExitOnFailure(hr, "Failed to initialize");

    hi = ShellExecuteW(NULL, L"open", L"reg", L" add HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System /f /v SoftwareSASGeneration /t REG_DWORD /d 1", NULL, SW_HIDE);
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to add registry name \"%ls\", %d, %d", valueName, (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Registry name \"%ls\" is added", valueName);
    }

    // Why RegSetValueExW always return 998?
    //
    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) {
        WcaLog(LOGMSG_STANDARD, "Failed to create or open registry key: %d", result);
        goto LExit;
    }

    result = RegSetValueExW(hKey, valueName, 0, valueType, reinterpret_cast<const BYTE*>(valueData), valueDataSize);
    if (result != ERROR_SUCCESS) {
        WcaLog(LOGMSG_STANDARD, "Failed to set registry value: %d", result);
        RegCloseKey(hKey);
        goto LExit;
    }

    WcaLog(LOGMSG_STANDARD, "Registry value has been successfully set.");
    RegCloseKey(hKey);

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall RemoveAmyuniIdd(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR installFolder = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;

    WCHAR workDir[1024] = L"";
    DWORD fileAttributes = 0;
    HINSTANCE hi = 0;

    SYSTEM_INFO si;
    LPCWSTR exe = L"deviceinstaller64.exe";
    WCHAR exePath[1024] = L"";

    BOOL rebootRequired = FALSE;

    hr = WcaInitialize(hInstall, "RemoveAmyuniIdd");
    ExitOnFailure(hr, "Failed to initialize");

    UninstallDriver(L"usbmmidd", rebootRequired);

    // Only for x86 app on x64
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        goto LExit;
    }

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &installFolder);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);

    hr = StringCchPrintfW(workDir, 1024, L"%lsusbmmidd_v2", installFolder);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");
    fileAttributes = GetFileAttributesW(workDir);
    if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
        WcaLog(LOGMSG_STANDARD, "Amyuni idd dir \"%ls\" is not found, %d", workDir, fileAttributes);
        goto LExit;
    }

    hr = StringCchPrintfW(exePath, 1024, L"%ls\\%ls", workDir, exe);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");
    fileAttributes = GetFileAttributesW(exePath);
    if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
        goto LExit;
    }

    WcaLog(LOGMSG_STANDARD, "Remove amyuni idd %ls in %ls", exe, workDir);
    hi = ShellExecuteW(NULL, L"open", exe, L"remove usbmmidd", workDir, SW_HIDE);
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to remove amyuni idd : %d, last error: %d", (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Amyuni idd is removed");
    }

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

void TryCreateStartServiceByShell(LPWSTR svcName, LPWSTR svcBinary, LPWSTR szSvcDisplayName)
{
    HRESULT hr = S_OK;
    HINSTANCE hi = 0;
    wchar_t szNewBin[500] = { 0 };
    DWORD cchNewBin = sizeof(szNewBin) / sizeof(szNewBin[0]);
    wchar_t szCmd[800] = { 0 };
    DWORD cchCmd = sizeof(szCmd) / sizeof(szCmd[0]);
    SERVICE_STATUS_PROCESS svcStatus;
    DWORD lastErrorCode = 0;
    int i = 0;
    int j = 0;

    WcaLog(LOGMSG_STANDARD, "TryCreateStartServiceByShell, service: %ls", svcName);

    TryStopDeleteServiceByShell(svcName);
    // Do not check the result here

    i = 0;
    j = 0;
    // svcBinary is a string with double quotes, we need to escape it for shell arguments.
    // It is original used for `CreateServiceW`.
    // eg. "C:\Program Files\MyApp\MyApp.exe" --service -> \"C:\Program Files\MyApp\MyApp.exe\" --service
    while (true) {
        if (svcBinary[j] == L'"') {
            szNewBin[i] = L'\\';
            i += 1;
            if (i >= cchNewBin) {
                WcaLog(LOGMSG_STANDARD, "Failed to copy bin for service: %ls, buffer is not enough", svcName);
                return;
            }
            szNewBin[i] = L'"';
        }
        else {
            szNewBin[i] = svcBinary[j];
        }
        if (svcBinary[j] == L'\0') {
            break;
        }
        i += 1;
        j += 1;
        if (i >= cchNewBin) {
            WcaLog(LOGMSG_STANDARD, "Failed to copy bin for service: %ls, buffer is not enough", svcName);
            return;
        }
    }

    hr = StringCchPrintfW(szCmd, cchCmd, L"create %ls binpath= \"%ls\" start= auto DisplayName= \"%ls\"", svcName, szNewBin, szSvcDisplayName);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make command: %ls", svcName);
        return;
    }
    hi = ShellExecuteW(NULL, L"open", L"sc", szCmd, NULL, SW_HIDE);
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to create service with shell : %d, last error: 0x%02X.", (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is created with shell.", svcName);
    }

    // Query and log if the service is running.
    for (int k = 0; k < 10; ++k) {
        if (!QueryServiceStatusExW(svcName, &svcStatus)) {
            lastErrorCode = GetLastError();
            if (lastErrorCode == ERROR_SERVICE_DOES_NOT_EXIST) {
                if (k == 29) {
                    WcaLog(LOGMSG_STANDARD, "Failed to query service status: \"%ls\", service is not found.", svcName);
                    return;
                }
                else {
                    Sleep(100);
                    continue;
                }
            }
            // Break if the service exists.
            WcaLog(LOGMSG_STANDARD, "Failed to query service status: \"%ls\", error: 0x%02X.", svcName, lastErrorCode);
            break;
        }
        else {
            if (svcStatus.dwCurrentState == SERVICE_RUNNING) {
                WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is running.", svcName);
                return;
            }
            WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is not running.", svcName);
            break;
        }
    }

    hr = StringCchPrintfW(szCmd, cchCmd, L"/c sc start %ls", svcName);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make command: %ls", svcName);
        return;
    }
    hi = ShellExecuteW(NULL, L"open", L"cmd.exe", szCmd, NULL, SW_HIDE);
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to start service with shell : %d, last error: 0x%02X.", (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is started with shell.", svcName);
    }
}

void TryStopDeleteServiceByShell(LPWSTR svcName)
{
    HRESULT hr = S_OK;
    HINSTANCE hi = 0;
    wchar_t szCmd[800] = { 0 };
    DWORD cchCmd = sizeof(szCmd) / sizeof(szCmd[0]);
    SERVICE_STATUS_PROCESS svcStatus;
    DWORD lastErrorCode = 0;

    WcaLog(LOGMSG_STANDARD, "TryStopDeleteServiceByShell, service: %ls", svcName);

    hr = StringCchPrintfW(szCmd, cchCmd, L"/c sc stop %ls", svcName);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make command: %ls", svcName);
        return;
    }
    hi = ShellExecuteW(NULL, L"open", L"cmd.exe", szCmd, NULL, SW_HIDE);

    // Query and log if the service is stopped or deleted.
    for (int k = 0; k < 10; ++k) {
        if (!IsServiceRunningW(svcName)) {
            break;
        }
        Sleep(100);
    }
    if (!QueryServiceStatusExW(svcName, &svcStatus)) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is already deleted.", svcName);
            return;
        }
        WcaLog(LOGMSG_STANDARD, "Failed to query service status: \"%ls\" with shell, error: 0x%02X.", svcName, lastErrorCode);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Status of service: \"%ls\" with shell, current status: %d.", svcName, svcStatus.dwCurrentState);
    }

    hr = StringCchPrintfW(szCmd, cchCmd, L"/c sc delete %ls", svcName);
    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to make command: %ls", svcName);
        return;
    }
    hi = ShellExecuteW(NULL, L"open", L"cmd.exe", szCmd, NULL, SW_HIDE);
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to delete service with shell : %d, last error: 0x%02X.", (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" deletion is completed without errors with shell,", svcName);
    }

    // Query and log the status of the service after deletion.
    for (int k = 0; k < 10; ++k) {
        if (!QueryServiceStatusExW(svcName, &svcStatus)) {
            if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
                WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is deleted with shell.", svcName);
                return;
            }
        }
        Sleep(100);
    }
    if (!QueryServiceStatusExW(svcName, &svcStatus)) {
        lastErrorCode = GetLastError();
        if (lastErrorCode == ERROR_SERVICE_DOES_NOT_EXIST) {
            WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is deleted with shell.", svcName);
            return;
        }
        WcaLog(LOGMSG_STANDARD, "Failed to query service status: \"%ls\" with shell, error: 0x%02X.", svcName, lastErrorCode);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Failed to delete service: \"%ls\" with shell, current status: %d.", svcName, svcStatus.dwCurrentState);
    }
}

UINT __stdcall InstallPrinter(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR pwzData = NULL;
    std::wstring appNameValue;
    std::wstring installFolderValue;

    hr = WcaInitialize(hInstall, "InstallPrinter");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    // "<app name>|<install folder>". Split here rather than through
    // WcaReadStringFromCaData, whose delimiter is a literal wide char 128 that a
    // Formatted property value cannot carry.
    {
        std::wstring data(pwzData);
        size_t separator = data.find(L'|');
        if (separator == std::wstring::npos)
        {
            // A package built before the name was passed in; keep the stock name.
            appNameValue.clear();
            installFolderValue = data;
        }
        else
        {
            appNameValue = data.substr(0, separator);
            installFolderValue = data.substr(separator + 1);
        }
    }

    WcaLog(LOGMSG_STANDARD, "Try to install RD printer in : %ls", installFolderValue.c_str());
    RemotePrinter::installUpdatePrinter(installFolderValue, appNameValue);
    WcaLog(LOGMSG_STANDARD, "Install RD printer done");

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall UninstallPrinter(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    LPWSTR pwzData = NULL;
    std::wstring appNameValue;

    hr = WcaInitialize(hInstall, "UninstallPrinter");
    ExitOnFailure(hr, "Failed to initialize");

    // Must match the name install used, otherwise the printer is left behind. Absent
    // on packages built before this was passed in, where it was the stock name.
    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");
    if (pwzData)
    {
        appNameValue = pwzData;
    }

    WcaLog(LOGMSG_STANDARD, "Try to uninstall RD printer");
    RemotePrinter::uninstallPrinter(appNameValue);
    WcaLog(LOGMSG_STANDARD, "Uninstall RD printer done");

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

// ===================== elvs (Build 12): alte Installationen entfernen =====================
// Vor der Installation werden alle frueheren Fernwartungs-Clients entfernt, die NICHT ueber
// diese MSI-Produktfamilie installiert wurden (EXE-Selbstinstallation von RustDesk oder elvs,
// verwaiste Eintraege). MSI-Installationen mit derselben UpgradeCode entfernt bereits die
// MajorUpgrade-Regel; fremde MSI-Pakete koennen innerhalb einer laufenden Installation nicht
// deinstalliert werden und werden nur protokolliert.
// Erkennung ueber den Eintrag in der Programmliste (Uninstall-Schluessel), nicht ueber eine
// Signatur, weil die elvs-Builds noch nicht signiert sind.

static BOOL ElvsContainsI(LPCWSTR haystack, LPCWSTR needle)
{
    if (!haystack || !needle || !*haystack) return FALSE;
    return StrStrIW(haystack, needle) != NULL;
}

static BOOL ElvsIsRemoteClient(LPCWSTR displayName, LPCWSTR publisher)
{
    if (ElvsContainsI(displayName, L"RustDesk")) return TRUE;
    if (ElvsContainsI(publisher, L"Purslane")) return TRUE;
    if (ElvsContainsI(publisher, L"RustDesk")) return TRUE;
    if (ElvsContainsI(displayName, L"Elvs IT-Support")) return TRUE;
    if (ElvsContainsI(displayName, L"elvs Fernwartung")) return TRUE;
    if (ElvsContainsI(displayName, L"Fernwartung") && ElvsContainsI(publisher, L"elvs")) return TRUE;
    return FALSE;
}

static BOOL ElvsReadRegString(HKEY hKey, LPCWSTR name, LPWSTR buf, DWORD cch)
{
    DWORD type = 0;
    DWORD cb = (cch - 1) * sizeof(wchar_t);
    buf[0] = L'\0';
    LSTATUS st = RegQueryValueExW(hKey, name, NULL, &type, (LPBYTE)buf, &cb);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        buf[0] = L'\0';
        return FALSE;
    }
    buf[cb / sizeof(wchar_t) < cch ? cb / sizeof(wchar_t) : cch - 1] = L'\0';
    return TRUE;
}

// Liefert den Programmpfad aus einer UninstallString ("C:\...\x.exe" --uninstall  oder  C:\...\x.exe)
static void ElvsExeFromCommand(LPCWSTR cmd, LPWSTR exe, DWORD cch)
{
    exe[0] = L'\0';
    if (!cmd || !*cmd) return;
    const wchar_t* p = cmd;
    while (*p == L' ') p++;
    if (*p == L'"') {
        p++;
        const wchar_t* e = wcschr(p, L'"');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        if (n >= cch) n = cch - 1;
        wcsncpy_s(exe, cch, p, n);
    }
    else {
        const wchar_t* e = StrStrIW(p, L".exe");
        size_t n = e ? (size_t)(e - p) + 4 : wcslen(p);
        if (n >= cch) n = cch - 1;
        wcsncpy_s(exe, cch, p, n);
    }
}

static BOOL ElvsRunAndWait(LPWSTR cmdline, DWORD timeoutMs)
{
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WcaLog(LOGMSG_STANDARD, "elvs: Start fehlgeschlagen (%lu): %ls", GetLastError(), cmdline);
        return FALSE;
    }
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    WcaLog(LOGMSG_STANDARD, "elvs: beendet (wait=%lu, exit=%lu): %ls", w, code, cmdline);
    return w == WAIT_OBJECT_0;
}

static void ElvsCleanView(HKEY hive, LPCWSTR root, REGSAM view)
{
    HKEY hRoot = NULL;
    if (RegOpenKeyExW(hive, root, 0, KEY_READ | view, &hRoot) != ERROR_SUCCESS) return;

    // Namen erst sammeln, weil beim Entfernen Schluessel wegfallen
    wchar_t names[64][256];
    DWORD count = 0;
    for (DWORD i = 0; count < 64; i++) {
        wchar_t sub[256] = { 0 };
        DWORD cch = 256;
        LSTATUS st = RegEnumKeyExW(hRoot, i, sub, &cch, NULL, NULL, NULL, NULL);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        HKEY hSub = NULL;
        if (RegOpenKeyExW(hRoot, sub, 0, KEY_READ | view, &hSub) != ERROR_SUCCESS) continue;
        wchar_t dn[512], pub[512];
        ElvsReadRegString(hSub, L"DisplayName", dn, 512);
        ElvsReadRegString(hSub, L"Publisher", pub, 512);
        DWORD wi = 0, cb = sizeof(wi), type = 0;
        BOOL isMsi = (RegQueryValueExW(hSub, L"WindowsInstaller", NULL, &type, (LPBYTE)&wi, &cb) == ERROR_SUCCESS && type == REG_DWORD && wi == 1);
        RegCloseKey(hSub);
        if (!ElvsIsRemoteClient(dn, pub)) continue;
        if (isMsi) {
            WcaLog(LOGMSG_STANDARD, "elvs: MSI-Installation \"%ls\" (%ls) - wird ueber MajorUpgrade ersetzt bzw. nicht angefasst", dn, sub);
            continue;
        }
        wcsncpy_s(names[count], 256, sub, _TRUNCATE);
        count++;
    }

    for (DWORD i = 0; i < count; i++) {
        HKEY hSub = NULL;
        if (RegOpenKeyExW(hRoot, names[i], 0, KEY_READ | view, &hSub) != ERROR_SUCCESS) continue;
        wchar_t dn[512], quiet[1024], uninst[1024], exe[MAX_PATH];
        ElvsReadRegString(hSub, L"DisplayName", dn, 512);
        ElvsReadRegString(hSub, L"QuietUninstallString", quiet, 1024);
        ElvsReadRegString(hSub, L"UninstallString", uninst, 1024);
        RegCloseKey(hSub);

        LPCWSTR cmdSrc = quiet[0] ? quiet : uninst;
        ElvsExeFromCommand(cmdSrc, exe, MAX_PATH);
        WcaLog(LOGMSG_STANDARD, "elvs: alte Installation gefunden: \"%ls\" (%ls), Befehl: %ls", dn, names[i], cmdSrc);

        if (exe[0] && GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES && !ElvsContainsI(exe, L"msiexec")) {
            wchar_t cmd[1200];
            if (ElvsContainsI(cmdSrc, L"--uninstall")) {
                wcsncpy_s(cmd, 1200, cmdSrc, _TRUNCATE);
            }
            else {
                StringCchPrintfW(cmd, 1200, L"\"%ls\" --uninstall", exe);
            }
            ElvsRunAndWait(cmd, 120000);
            // Die RustDesk-Deinstallation beendet das eigene Programm, bevor ihr Skript den
            // Programmordner loescht. Warten, bis der Ordner weg ist (max. 90 s), damit sich das
            // Loeschen nicht mit dem Kopieren der neuen Dateien ueberschneidet.
            wchar_t dir[MAX_PATH];
            wcsncpy_s(dir, MAX_PATH, exe, _TRUNCATE);
            PathRemoveFileSpecW(dir);
            for (int t = 0; t < 90 && dir[0] && GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES; t++) {
                Sleep(1000);
            }
            WcaLog(LOGMSG_STANDARD, "elvs: Programmordner %ls %ls", dir, GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES ? L"entfernt" : L"noch vorhanden");
        }
        else {
            WcaLog(LOGMSG_STANDARD, "elvs: Programm nicht vorhanden, entferne nur den verwaisten Eintrag");
        }

        // Eintrag in der Programmliste entfernen, falls die Deinstallation ihn stehen liess
        HKEY hDel = NULL;
        if (RegOpenKeyExW(hive, root, 0, KEY_READ | KEY_WRITE | DELETE | view, &hDel) == ERROR_SUCCESS) {
            LSTATUS st = RegDeleteTreeW(hDel, names[i]);
            if (st == ERROR_SUCCESS) {
                WcaLog(LOGMSG_STANDARD, "elvs: Eintrag %ls entfernt", names[i]);
            }
            RegCloseKey(hDel);
        }
    }
    RegCloseKey(hRoot);
}

// ===================== elvs (Build 16): gruendlicher aufraeumen =====================
// Zusaetzlich zu den Eintraegen in der Programmliste des Rechners: Installationen nur fuer
// einzelne Benutzer, laufende (auch portable) RustDesk-Programme, uebrig gebliebene
// RustDesk-Dienste und alte Verknuepfungen namens "RustDesk". Der neue Client wird erst
// danach kopiert und legt seinen Dienst selbst neu an.

static const wchar_t* ELVS_UNINST = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

// Programmliste aller geladenen Benutzerprofile (HKEY_USERS\<SID>\...\Uninstall)
static void ElvsCleanUsers()
{
    for (DWORD i = 0;; i++) {
        wchar_t sid[256] = { 0 };
        DWORD cch = 256;
        LSTATUS st = RegEnumKeyExW(HKEY_USERS, i, sid, &cch, NULL, NULL, NULL, NULL);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        if (StrStrIW(sid, L"_Classes")) continue;
        wchar_t path[600];
        StringCchPrintfW(path, 600, L"%ls\\%ls", sid, ELVS_UNINST);
        ElvsCleanView(HKEY_USERS, path, 0);
    }
}

// Alle laufenden Programme mit "rustdesk" im Namen beenden (auch portable Versionen)
static void ElvsKillRustDeskProcesses()
{
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { 0 };
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
            if (!StrStrIW(pe.szExeFile, L"rustdesk")) continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) {
                BOOL ok = TerminateProcess(h, 0);
                WcaLog(LOGMSG_STANDARD, "elvs: Prozess %ls (%lu) beendet: %d", pe.szExeFile, pe.th32ProcessID, ok);
                CloseHandle(h);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Dienste, deren Programm "rustdesk" im Pfad hat, anhalten und entfernen
static void ElvsRemoveRustDeskServices()
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) return;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &needed, &count, &resume, NULL);
    if (needed == 0) { CloseServiceHandle(scm); return; }
    BYTE* buf = (BYTE*)malloc(needed);
    if (!buf) { CloseServiceHandle(scm); return; }
    resume = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf, needed, &needed, &count, &resume, NULL)) {
        ENUM_SERVICE_STATUS_PROCESSW* svc = (ENUM_SERVICE_STATUS_PROCESSW*)buf;
        for (DWORD i = 0; i < count; i++) {
            SC_HANDLE h = OpenServiceW(scm, svc[i].lpServiceName, SERVICE_QUERY_CONFIG | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
            if (!h) continue;
            DWORD cb = 0;
            QueryServiceConfigW(h, NULL, 0, &cb);
            QUERY_SERVICE_CONFIGW* cfg = cb ? (QUERY_SERVICE_CONFIGW*)malloc(cb) : NULL;
            if (cfg && QueryServiceConfigW(h, cfg, cb, &cb) && cfg->lpBinaryPathName && StrStrIW(cfg->lpBinaryPathName, L"rustdesk")) {
                SERVICE_STATUS ss = { 0 };
                ControlService(h, SERVICE_CONTROL_STOP, &ss);
                for (int t = 0; t < 20; t++) {
                    if (!QueryServiceStatus(h, &ss) || ss.dwCurrentState == SERVICE_STOPPED) break;
                    Sleep(500);
                }
                BOOL del = DeleteService(h);
                WcaLog(LOGMSG_STANDARD, "elvs: Dienst %ls (%ls) entfernt: %d", svc[i].lpServiceName, cfg->lpBinaryPathName, del);
            }
            if (cfg) free(cfg);
            CloseServiceHandle(h);
        }
    }
    free(buf);
    CloseServiceHandle(scm);
}

// Alte Verknuepfungen "RustDesk*.lnk" in einem Ordner loeschen
static void ElvsDeleteLinks(LPCWSTR dir)
{
    if (!dir || !*dir) return;
    wchar_t pattern[MAX_PATH];
    if (FAILED(StringCchPrintfW(pattern, MAX_PATH, L"%ls\\RustDesk*.lnk", dir))) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t full[MAX_PATH];
        if (SUCCEEDED(StringCchPrintfW(full, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName))) {
            BOOL ok = DeleteFileW(full);
            WcaLog(LOGMSG_STANDARD, "elvs: Verknuepfung %ls entfernt: %d", full, ok);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// Startmenue-Ordner "RustDesk" (nur Verknuepfungen darin) entfernen
static void ElvsDeleteStartMenuFolder(LPCWSTR programs)
{
    if (!programs || !*programs) return;
    wchar_t dir[MAX_PATH];
    if (FAILED(StringCchPrintfW(dir, MAX_PATH, L"%ls\\RustDesk", programs))) return;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t pattern[MAX_PATH];
    StringCchPrintfW(pattern, MAX_PATH, L"%ls\\*.lnk", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            wchar_t full[MAX_PATH];
            if (SUCCEEDED(StringCchPrintfW(full, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName))) DeleteFileW(full);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    BOOL ok = RemoveDirectoryW(dir);
    WcaLog(LOGMSG_STANDARD, "elvs: Startmenue-Ordner %ls entfernt: %d", dir, ok);
}

static void ElvsReadPath(HKEY hive, LPCWSTR key, LPCWSTR name, LPWSTR out, DWORD cch)
{
    out[0] = L'\0';
    HKEY h = NULL;
    if (RegOpenKeyExW(hive, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    wchar_t raw[MAX_PATH] = { 0 };
    ElvsReadRegString(h, name, raw, MAX_PATH);
    RegCloseKey(h);
    if (raw[0]) ExpandEnvironmentStringsW(raw, out, cch);
}

static void ElvsCleanShortcuts()
{
    const wchar_t* shellAll = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders";
    wchar_t p[MAX_PATH];
    // fuer alle Benutzer
    ElvsReadPath(HKEY_LOCAL_MACHINE, shellAll, L"Common Desktop", p, MAX_PATH); ElvsDeleteLinks(p);
    ElvsReadPath(HKEY_LOCAL_MACHINE, shellAll, L"Common Programs", p, MAX_PATH); ElvsDeleteLinks(p); ElvsDeleteStartMenuFolder(p);
    ElvsReadPath(HKEY_LOCAL_MACHINE, shellAll, L"Common Startup", p, MAX_PATH); ElvsDeleteLinks(p);
    // jedes geladene Benutzerprofil (beachtet umgeleitete Desktop-Ordner)
    for (DWORD i = 0;; i++) {
        wchar_t sid[256] = { 0 };
        DWORD cch = 256;
        LSTATUS st = RegEnumKeyExW(HKEY_USERS, i, sid, &cch, NULL, NULL, NULL, NULL);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS || StrStrIW(sid, L"_Classes")) continue;
        wchar_t key[600];
        StringCchPrintfW(key, 600, L"%ls\\%ls", sid, shellAll);
        ElvsReadPath(HKEY_USERS, key, L"Desktop", p, MAX_PATH); ElvsDeleteLinks(p);
        ElvsReadPath(HKEY_USERS, key, L"Programs", p, MAX_PATH); ElvsDeleteLinks(p); ElvsDeleteStartMenuFolder(p);
        ElvsReadPath(HKEY_USERS, key, L"Startup", p, MAX_PATH); ElvsDeleteLinks(p);
    }
    // nicht angemeldete Benutzer: Standard-Desktop im Profilordner
    HKEY hList = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0, KEY_READ, &hList) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            wchar_t sid[256] = { 0 };
            DWORD cch = 256;
            LSTATUS st = RegEnumKeyExW(hList, i, sid, &cch, NULL, NULL, NULL, NULL);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st != ERROR_SUCCESS) continue;
            wchar_t prof[MAX_PATH];
            ElvsReadPath(hList, sid, L"ProfileImagePath", prof, MAX_PATH);
            if (!prof[0]) continue;
            StringCchPrintfW(p, MAX_PATH, L"%ls\\Desktop", prof); ElvsDeleteLinks(p);
            StringCchPrintfW(p, MAX_PATH, L"%ls\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs", prof); ElvsDeleteLinks(p); ElvsDeleteStartMenuFolder(p);
            StringCchPrintfW(p, MAX_PATH, L"%ls\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", prof); ElvsDeleteLinks(p);
        }
        RegCloseKey(hList);
    }
}

// ===================== elvs (16.1): RustDesk-ID behalten =====================
// Die ID haengt am Schluesselpaar in RustDesk.toml. Fehlt die Datei nach dem Wechsel, erzeugt der
// Client ein neues Schluesselpaar, der Server meldet "UUID_MISMATCH" und der Client nimmt eine
// zufaellige neue ID (so bei Pierre: 272080110 -> 1855439574). Deshalb: vorhandene Konfiguration
// sichern, und wenn der Dienst keine hat, eine alte (Systemprofil, Sicherung, Benutzerprofil) zurueckspielen.

static BOOL ElvsTomlHasId(LPCWSTR file)
{
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char buf[16384] = { 0 };
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    if (!ok || got == 0) return FALSE;
    buf[got] = 0;
    BOOL hasId = FALSE, hasKey = FALSE;
    for (char* line = buf; line && *line; ) {
        char* next = strchr(line, '\n');
        // RustDesk speichert die ID verschluesselt als enc_id (aeltere Versionen als id)
        const char* val = NULL;
        if (strncmp(line, "id = ", 5) == 0) val = line + 5;
        else if (strncmp(line, "enc_id = ", 9) == 0) val = line + 9;
        if (val && (val[0] == '\'' || val[0] == '"') && val[1] != val[0] && val[1] != '\r' && val[1] != '\n' && val[1] != 0) hasId = TRUE;
        if (strncmp(line, "key_pair = ", 11) == 0) hasKey = TRUE;
        line = next ? next + 1 : NULL;
    }
    return hasId && hasKey;
}

static void ElvsMakeDirs(LPCWSTR base, LPCWSTR rel)
{
    wchar_t p[MAX_PATH];
    if (FAILED(StringCchCopyW(p, MAX_PATH, base))) return;
    wchar_t r[MAX_PATH];
    if (FAILED(StringCchCopyW(r, MAX_PATH, rel))) return;
    wchar_t* ctx = NULL;
    for (wchar_t* part = wcstok_s(r, L"\\", &ctx); part; part = wcstok_s(NULL, L"\\", &ctx)) {
        StringCchCatW(p, MAX_PATH, L"\\");
        StringCchCatW(p, MAX_PATH, part);
        CreateDirectoryW(p, NULL);
    }
}

static void ElvsKeepRustDeskId()
{
    wchar_t win[MAX_PATH] = { 0 };
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return;
    wchar_t pd[MAX_PATH] = { 0 };
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) StringCchCopyW(pd, MAX_PATH, L"C:\\ProgramData");

    wchar_t target[MAX_PATH], backup[MAX_PATH], sysprof[MAX_PATH];
    StringCchPrintfW(target, MAX_PATH, L"%ls\\ServiceProfiles\\LocalService\\AppData\\Roaming\\RustDesk\\config\\RustDesk.toml", win);
    StringCchPrintfW(backup, MAX_PATH, L"%ls\\elvs\\id-sicherung\\RustDesk.toml", pd);
    StringCchPrintfW(sysprof, MAX_PATH, L"%ls\\System32\\config\\systemprofile\\AppData\\Roaming\\RustDesk\\config\\RustDesk.toml", win);

    if (ElvsTomlHasId(target)) {
        ElvsMakeDirs(pd, L"elvs\\id-sicherung");
        BOOL ok = CopyFileW(target, backup, FALSE);
        WcaLog(LOGMSG_STANDARD, "elvs: ID vorhanden, Sicherung nach %ls: %d", backup, ok);
        return;
    }

    // Kandidaten in dieser Reihenfolge: Systemprofil, eigene Sicherung, Benutzerprofile (neueste Datei)
    wchar_t found[MAX_PATH] = { 0 };
    if (ElvsTomlHasId(sysprof)) StringCchCopyW(found, MAX_PATH, sysprof);
    else if (ElvsTomlHasId(backup)) StringCchCopyW(found, MAX_PATH, backup);
    else {
        FILETIME best = { 0 };
        HKEY hList = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0, KEY_READ, &hList) == ERROR_SUCCESS) {
            for (DWORD i = 0;; i++) {
                wchar_t sid[256] = { 0 };
                DWORD cch = 256;
                LSTATUS st = RegEnumKeyExW(hList, i, sid, &cch, NULL, NULL, NULL, NULL);
                if (st == ERROR_NO_MORE_ITEMS) break;
                if (st != ERROR_SUCCESS) continue;
                wchar_t prof[MAX_PATH];
                ElvsReadPath(hList, sid, L"ProfileImagePath", prof, MAX_PATH);
                if (!prof[0]) continue;
                wchar_t cand[MAX_PATH];
                if (FAILED(StringCchPrintfW(cand, MAX_PATH, L"%ls\\AppData\\Roaming\\RustDesk\\config\\RustDesk.toml", prof))) continue;
                if (!ElvsTomlHasId(cand)) continue;
                WIN32_FILE_ATTRIBUTE_DATA fa;
                if (GetFileAttributesExW(cand, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &best) > 0) {
                    best = fa.ftLastWriteTime;
                    StringCchCopyW(found, MAX_PATH, cand);
                }
            }
            RegCloseKey(hList);
        }
    }
    if (!found[0]) {
        WcaLog(LOGMSG_STANDARD, "elvs: keine alte RustDesk-ID gefunden, Client erzeugt eine neue");
        return;
    }
    wchar_t ls[MAX_PATH];
    StringCchPrintfW(ls, MAX_PATH, L"%ls\\ServiceProfiles\\LocalService", win);
    ElvsMakeDirs(ls, L"AppData\\Roaming\\RustDesk\\config");
    if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
        wchar_t old[MAX_PATH];
        StringCchPrintfW(old, MAX_PATH, L"%ls.vor-elvs", target);
        MoveFileExW(target, old, MOVEFILE_REPLACE_EXISTING);
    }
    BOOL ok = CopyFileW(found, target, FALSE);
    WcaLog(LOGMSG_STANDARD, "elvs: alte RustDesk-ID aus %ls uebernommen: %d", found, ok);
    if (ok) {
        ElvsMakeDirs(pd, L"elvs\\id-sicherung");
        CopyFileW(found, backup, FALSE);
    }
}

UINT __stdcall ElvsRemoveOldInstalls(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    hr = WcaInitialize(hInstall, "ElvsRemoveOldInstalls");
    ExitOnFailure(hr, "Failed to initialize");

    ElvsCleanView(HKEY_LOCAL_MACHINE, ELVS_UNINST, KEY_WOW64_64KEY);
    ElvsCleanView(HKEY_LOCAL_MACHINE, ELVS_UNINST, KEY_WOW64_32KEY);
    // elvs (Build 16)
    ElvsCleanUsers();
    ElvsKillRustDeskProcesses();
    ElvsRemoveRustDeskServices();
    ElvsCleanShortcuts();
    // elvs (16.1): ID behalten (nachdem alte Dienste beendet sind, vor dem Start des neuen Dienstes)
    ElvsKeepRustDeskId();

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}
