/*
This code is from http://omeg.pl/blog/2014/01/running-processes-on-the-winlogon-desktop/

It has been modified, refer to https://gist.github.com/jay/be6b18a2eece726922bb/revisions

- Use stub functions for logf and log_init since I can't find log.h.
- Correct the function signature for ServiceMain.
- Return 0 from main.
- Bug: Close currentToken instead of currentProcess in WorkerThread.
- Close process, thread and newToken handles in WorkerThread.

----------
This service works to run a command on the winlogon desktop during switch user. It may take several
seconds before the command runs, not sure why. The service stops after it runs the command.

gcc -o QTestService QTestService.c

sc create QTestService binPath= X:\code\winlogon\QTestService\QTestService.exe start= demand
sc start QTestService
*/

#include <windows.h>

//#include "log.h"
#include <stdio.h>

FILE *g_fp;

#define log_init(unknown1, unknown2) do { \
g_fp = fopen("X:\\code\\winlogon\\QTestService\\QTestService.log", "a"); \
} while(0)

#define logf(...) do { \
if(g_fp) fprintf(g_fp, __VA_ARGS__); fprintf(g_fp, "\n"); fflush(g_fp); \
} while(0)

#define SERVICE_NAME TEXT("QTestService")

SERVICE_STATUS g_Status;
SERVICE_STATUS_HANDLE g_StatusHandle;
HANDLE g_ConsoleEvent;
DWORD g_TargetSessionId;

void ServiceMain(unsigned long argc, TCHAR *argv[]);
DWORD ControlHandlerEx(DWORD controlCode, DWORD eventType, void *eventData, void *context);

WCHAR *g_SessionEventName[] = {
    L"<invalid>",
    L"WTS_CONSOLE_CONNECT",
    L"WTS_CONSOLE_DISCONNECT",
    L"WTS_REMOTE_CONNECT",
    L"WTS_REMOTE_DISCONNECT",
    L"WTS_SESSION_LOGON",
    L"WTS_SESSION_LOGOFF",
    L"WTS_SESSION_LOCK",
    L"WTS_SESSION_UNLOCK",
    L"WTS_SESSION_REMOTE_CONTROL",
    L"WTS_SESSION_CREATE",
    L"WTS_SESSION_TERMINATE"
};

// Entry point.
int main(int argc, TCHAR *argv[])
{
    SERVICE_TABLE_ENTRY serviceTable[] = {
        {SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };

    log_init(NULL, TEXT("qservice"));
    logf("main: start");
    StartServiceCtrlDispatcher(serviceTable);
    logf("main: end");
    return 0;
}

DWORD WINAPI WorkerThread(void *param)
{
    TCHAR *cmdline;
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    HANDLE newToken;
    DWORD sessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();

    cmdline = (TCHAR*) param;

    // Wait until the interactive session changes (to winlogon console).
    WaitForSingleObject(g_ConsoleEvent, INFINITE);

    // Get access token from ourselves.
    OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
    // Session ID is stored in the access token. For services it's normally 0.
    GetTokenInformation(currentToken, TokenSessionId, &sessionId, sizeof(sessionId), &size);
    logf("current session: %d", sessionId);

    // We need to create a primary token for CreateProcessAsUser.
    if (!DuplicateTokenEx(currentToken, TOKEN_ALL_ACCESS, NULL,
        SecurityImpersonation, TokenPrimary, &newToken))
    {
        perror("DuplicateToken");
        return GetLastError();
    }
    CloseHandle(currentToken);

    // g_TargetSessionId is set by SessionChange() handler after a WTS_CONSOLE_CONNECT event.
    // Its value is the new console session ID. In our case it's the "logon screen".
    sessionId = g_TargetSessionId;
    logf("Running process '%s' in session %d", cmdline, sessionId);
    // Change the session ID in the new access token to the target session ID.
    // This requires SeTcbPrivilege, but we're running as SYSTEM and have it.
    if (!SetTokenInformation(newToken, TokenSessionId, &sessionId, sizeof(sessionId)))
    {
        perror("SetTokenInformation(TokenSessionId)");
        return GetLastError();
    }

    // Create process with the new token.
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    // Don't forget to set the correct desktop.
    si.lpDesktop = TEXT("WinSta0\\Winlogon");
    if (!CreateProcessAsUser(newToken, 0, cmdline, 0, 0, 0, 0, 0, 0, &si, &pi))
    {
        perror("CreateProcessAsUser");
        return GetLastError();
    }
    CloseHandle(newToken);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return ERROR_SUCCESS;
}

void ServiceMain(unsigned long argc, TCHAR *argv[])
{
    TCHAR *cmdline = TEXT("cmd.exe");
    HANDLE workerHandle;

    logf("ServiceMain: start");
    g_Status.dwServiceType        = SERVICE_WIN32;
    g_Status.dwCurrentState       = SERVICE_START_PENDING;
    // SERVICE_ACCEPT_SESSIONCHANGE allows us to receive session change notifications.
    g_Status.dwControlsAccepted   = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        | SERVICE_ACCEPT_SESSIONCHANGE;
    g_Status.dwWin32ExitCode      = 0;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwCheckPoint         = 0;
    g_Status.dwWaitHint           = 0;
    g_StatusHandle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, ControlHandlerEx, NULL);
    if (g_StatusHandle == 0)
    {
        perror("RegisterServiceCtrlHandlerEx");
        goto stop;
    }

    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);

    // Create trigger event for the worker thread.
    g_ConsoleEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Start the worker thread.
    logf("Starting worker thread");
    workerHandle = CreateThread(NULL, 0, WorkerThread, cmdline, 0, NULL);
    if (!workerHandle)
    {
        perror("CreateThread");
        goto stop;
    }

    // Wait for the worker thread to exit.
    WaitForSingleObject(workerHandle, INFINITE);

stop:
    logf("exiting");
    g_Status.dwCurrentState = SERVICE_STOPPED;
    g_Status.dwWin32ExitCode = GetLastError();
    SetServiceStatus(g_StatusHandle, &g_Status);

    logf("ServiceMain: end");
    return;
}

void SessionChange(DWORD eventType, WTSSESSION_NOTIFICATION *sn)
{
    if (eventType < RTL_NUMBER_OF(g_SessionEventName))
        logf("SessionChange: %s, session ID %d", g_SessionEventName[eventType], sn->dwSessionId);
    else
        logf("SessionChange: <unknown event: %d>, session id %d", eventType, sn->dwSessionId);

    if (eventType == WTS_CONSOLE_CONNECT)
    {
        // Store the new session ID for the worker thread and signal the trigger event.
        g_TargetSessionId = sn->dwSessionId;
        SetEvent(g_ConsoleEvent);
    }
}

DWORD ControlHandlerEx(DWORD control, DWORD eventType, void *eventData, void *context)
{
    switch(control)
    {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_Status.dwWin32ExitCode = 0;
            g_Status.dwCurrentState = SERVICE_STOPPED;
            logf("stopping...");
            SetServiceStatus(g_StatusHandle, &g_Status);
            break;

        case SERVICE_CONTROL_SESSIONCHANGE:
            SessionChange(eventType, (WTSSESSION_NOTIFICATION*) eventData);
            break;

        default:
            logf("ControlHandlerEx: code 0x%x, event 0x%x", control, eventType);
            break;
    }

    return ERROR_SUCCESS;
}
