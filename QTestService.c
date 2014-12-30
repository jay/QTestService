/*
This is my fork of omeg's QTestService from his article 'Running processes on the Winlogon desktop'.

http://omeg.pl/blog/2014/01/running-processes-on-the-winlogon-desktop/
https://github.com/jay/QTestService

This service works to run a command on the winlogon desktop during switch user. It may take several
seconds before the command runs, not sure why. The service stops after it runs the command unless
you define LOOP_WORKER, in which case it continues to run and monitor for session connections.

gcc -o QTestService QTestService.c
gcc -DLOOP_WORKER -o QTestService QTestService.c

If you are making a Unicode build you must define both UNICODE and _UNICODE.

sc create QTestService binPath= X:\code\winlogon\QTestService\QTestService.exe start= demand
sc start QTestService
*/

#define   _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <tchar.h>
#include <stdio.h>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif


// begin: stuff for older mingw
#ifndef WTS_CONSOLE_CONNECT
#define WTS_CONSOLE_CONNECT                0x1
#define WTS_CONSOLE_DISCONNECT             0x2
#define WTS_REMOTE_CONNECT                 0x3
#define WTS_REMOTE_DISCONNECT              0x4
#define WTS_SESSION_LOGON                  0x5
#define WTS_SESSION_LOGOFF                 0x6
#define WTS_SESSION_LOCK                   0x7
#define WTS_SESSION_UNLOCK                 0x8
#define WTS_SESSION_REMOTE_CONTROL         0x9
typedef struct tagWTSSESSION_NOTIFICATION
{
    DWORD cbSize;
    DWORD dwSessionId;
} WTSSESSION_NOTIFICATION, *PWTSSESSION_NOTIFICATION;
#endif

#ifndef TOKEN_ADJUST_SESSIONID
#define TOKEN_ADJUST_SESSIONID   (0x0100)
#undef TOKEN_ALL_ACCESS
#define TOKEN_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED |\
                          TOKEN_ASSIGN_PRIMARY     |\
                          TOKEN_DUPLICATE          |\
                          TOKEN_IMPERSONATE        |\
                          TOKEN_QUERY              |\
                          TOKEN_QUERY_SOURCE       |\
                          TOKEN_ADJUST_PRIVILEGES  |\
                          TOKEN_ADJUST_GROUPS      |\
                          TOKEN_ADJUST_DEFAULT     |\
                          TOKEN_ADJUST_SESSIONID)
#endif

#ifndef RTL_NUMBER_OF
#define RTL_NUMBER_OF(A)   (sizeof(A)/sizeof((A)[0]))
#endif
// end: stuff for older mingw


FILE *g_log_fp;
CRITICAL_SECTION g_log_lock;

#ifdef _UNICODE
#define PRINT_VA(...)   fwprintf(g_log_fp, L##__VA_ARGS__)
#else
#define PRINT_VA(...)   fprintf(g_log_fp, __VA_ARGS__)
#endif

// This is meant to be called by the other function macros.
// GetLastError must be the first call to accurately get the last error!
#define plumb_logf(show_gle, ...) do { \
    if(g_log_fp) { \
        DWORD gle; \
        SYSTEMTIME t; \
        gle = GetLastError(); \
        EnterCriticalSection(&g_log_lock); \
        GetLocalTime(&t); \
        _ftprintf(g_log_fp, TEXT("%04u-%02u-%02u %02u:%02u:%02u.%03u - "), \
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds); \
        PRINT_VA(__VA_ARGS__); \
        if(show_gle) { \
            _ftprintf(g_log_fp, TEXT(", GetLastError: %I32u"), gle); \
        } \
        _ftprintf(g_log_fp, TEXT("\n")); \
        fflush(g_log_fp); \
        LeaveCriticalSection(&g_log_lock); \
    } \
} while(0)

// log and don't append last error
#define logf(...)   plumb_logf(FALSE, __VA_ARGS__)

// log and append last error from GetLastError
#define logf_gle(...)   plumb_logf(TRUE, __VA_ARGS__)


#define SERVICE_NAME TEXT("QTestService")

SERVICE_STATUS g_Status;
SERVICE_STATUS_HANDLE g_StatusHandle; // this isn't a regular handle it's a struct
HANDLE g_ConsoleEvent = INVALID_HANDLE_VALUE;
DWORD g_TargetSessionId;

void WINAPI ServiceMain(DWORD argc, TCHAR *argv[]);
DWORD WINAPI ControlHandlerEx(DWORD controlCode, DWORD eventType, void *eventData, void *context);

TCHAR *g_SessionEventName[] = {
    TEXT("<invalid>"),
    TEXT("WTS_CONSOLE_CONNECT"),
    TEXT("WTS_CONSOLE_DISCONNECT"),
    TEXT("WTS_REMOTE_CONNECT"),
    TEXT("WTS_REMOTE_DISCONNECT"),
    TEXT("WTS_SESSION_LOGON"),
    TEXT("WTS_SESSION_LOGOFF"),
    TEXT("WTS_SESSION_LOCK"),
    TEXT("WTS_SESSION_UNLOCK"),
    TEXT("WTS_SESSION_REMOTE_CONTROL"),
    TEXT("WTS_SESSION_CREATE"),
    TEXT("WTS_SESSION_TERMINATE")
};

// Return the control code's name or NULL if not recognized.
TCHAR *GetControlCodeName(DWORD code)
{
    switch(code)
    {
        case 1: return TEXT("SERVICE_CONTROL_STOP");
        case 2: return TEXT("SERVICE_CONTROL_PAUSE");
        case 3: return TEXT("SERVICE_CONTROL_CONTINUE");
        case 4: return TEXT("SERVICE_CONTROL_INTERROGATE");
        case 5: return TEXT("SERVICE_CONTROL_SHUTDOWN");
        case 6: return TEXT("SERVICE_CONTROL_PARAMCHANGE");
        case 7: return TEXT("SERVICE_CONTROL_NETBINDADD");
        case 8: return TEXT("SERVICE_CONTROL_NETBINDREMOVE");
        case 9: return TEXT("SERVICE_CONTROL_NETBINDENABLE");
        case 10: return TEXT("SERVICE_CONTROL_NETBINDDISABLE");
        case 11: return TEXT("SERVICE_CONTROL_DEVICEEVENT");
        case 12: return TEXT("SERVICE_CONTROL_HARDWAREPROFILECHANGE");
        case 13: return TEXT("SERVICE_CONTROL_POWEREVENT");
        case 14: return TEXT("SERVICE_CONTROL_SESSIONCHANGE");
        case 15: return TEXT("SERVICE_CONTROL_PRESHUTDOWN");
        case 16: return TEXT("SERVICE_CONTROL_TIMECHANGE");
        case 32: return TEXT("SERVICE_CONTROL_TRIGGEREVENT");
        case 64: return TEXT("SERVICE_CONTROL_USERMODEREBOOT");
    }

    return NULL;
}

// Entry point.
int main(int argc, char *argv[])
{
    SERVICE_TABLE_ENTRY serviceTable[] = {
        {SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };

    g_log_fp = _tfopen(TEXT("X:\\code\\winlogon\\QTestService\\QTestService.log"), TEXT("a"));
    if(g_log_fp)
    {
        _ftprintf(g_log_fp, TEXT("\n\n"));
        fflush(g_log_fp);
        InitializeCriticalSection(&g_log_lock);
    }

    logf("main: start");
    if(!StartServiceCtrlDispatcher(serviceTable))
    {
        logf_gle("StartServiceCtrlDispatcher failed");
    }
    logf("main: end");
    return 0;
}

DWORD WINAPI WorkerThread(void *param)
{
    DWORD errcode = ERROR_SUCCESS;
    TCHAR *cmdline = NULL;
    size_t cmdline_len = 0;
    PROCESS_INFORMATION pi = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, 0, 0 };
    STARTUPINFO si = { 0, };
    HANDLE newToken = INVALID_HANDLE_VALUE;
    DWORD sessionId = 0;
    DWORD size = 0;
    HANDLE currentToken = INVALID_HANDLE_VALUE;
    HANDLE currentProcess = GetCurrentProcess();

    if(!param)
    {
        logf("Error: No command line");
        errcode = ERROR_INVALID_PARAMETER;
        goto cleanup;
    }

    // Older MinGW doesn't have strnlen
    {
        void *p;
#ifdef _UNICODE
        p = wmemchr(
#else
        p = memchr(
#endif
            (TCHAR *)param, 0, 32768);

        if(!p)
        {
            logf("Error: Command line is too long");
            errcode = ERROR_INVALID_PARAMETER;
            goto cleanup;
        }

        cmdline_len = ((char *)p - (char *)param) / sizeof(TCHAR);
    }

    cmdline = malloc((cmdline_len + 1) * sizeof(TCHAR) );
    if(!cmdline)
    {
        logf("Error: malloc failed, not enough memory for command line");
        errcode = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

#ifdef LOOP_WORKER
loop:
#endif

    memcpy(cmdline, param, (cmdline_len + 1) * sizeof(TCHAR));

    // Wait until the interactive session changes (to winlogon console).
    logf("Waiting for event g_ConsoleEvent");
    WaitForSingleObject(g_ConsoleEvent, INFINITE);
    logf("Received event g_ConsoleEvent");

    // Get access token from ourselves.
    if(!OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken))
    {
        errcode = GetLastError();
        logf_gle("Error: OpenProcessToken failed");
        goto cleanup;
    }

    // Session ID is stored in the access token. For services it's normally 0.
    if(!GetTokenInformation(currentToken, TokenSessionId, &sessionId, sizeof(sessionId), &size))
    {
        errcode = GetLastError();
        logf_gle("Error: GetTokenInformation failed");
        goto cleanup;
    }
    logf("current session: %I32u", sessionId);

    // We need to create a primary token for CreateProcessAsUser.
    if (!DuplicateTokenEx(currentToken, TOKEN_ALL_ACCESS, NULL,
        SecurityImpersonation, TokenPrimary, &newToken))
    {
        errcode = GetLastError();
        logf_gle("Error: DuplicateTokenEx failed");
        goto cleanup;
    }

    // g_TargetSessionId is set by SessionChange() handler after a WTS_CONSOLE_CONNECT event.
    // Its value is the new console session ID. In our case it's the "logon screen".
    sessionId = g_TargetSessionId;
    logf("Running '%s' in session %I32u", cmdline, sessionId);

    // Change the session ID in the new access token to the target session ID.
    // This requires SeTcbPrivilege, but we're running as SYSTEM and have it.
    if (!SetTokenInformation(newToken, TokenSessionId, &sessionId, sizeof(sessionId)))
    {
        errcode = GetLastError();
        logf_gle("Error: SetTokenInformation failed");
        goto cleanup;
    }

    // Create process with the new token.
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    // Don't forget to set the correct desktop.
    si.lpDesktop = TEXT("WinSta0\\Winlogon");
    if (!CreateProcessAsUser(newToken, 0, cmdline, 0, 0, 0, 0, 0, 0, &si, &pi))
    {
        errcode = GetLastError();
        logf_gle("Error: CreateProcessAsUser failed");
        goto cleanup;
    }

cleanup:
    if(currentToken != INVALID_HANDLE_VALUE)
    {
        CloseHandle(currentToken);
        currentToken = INVALID_HANDLE_VALUE;
    }
    if(newToken != INVALID_HANDLE_VALUE)
    {
        CloseHandle(newToken);
        newToken = INVALID_HANDLE_VALUE;
    }
    if(pi.hThread != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pi.hThread);
        pi.hThread = INVALID_HANDLE_VALUE;
    }
    if(pi.hProcess != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pi.hProcess);
        pi.hProcess = INVALID_HANDLE_VALUE;
    }

    if(cmdline)
    {
#ifdef LOOP_WORKER
        goto loop;
#else
        free(cmdline);
#endif
    }
    return errcode;
}

void WINAPI ServiceMain(DWORD argc, TCHAR *argv[])
{
    DWORD errcode = ERROR_SUCCESS;
    TCHAR *cmdline = TEXT("cmd.exe");
    HANDLE workerHandle = INVALID_HANDLE_VALUE;

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
    if(!g_StatusHandle)
    {
        errcode = GetLastError();
        logf_gle("Error: RegisterServiceCtrlHandlerEx failed");
        // g_StatusHandle isn't set to INVALID_HANDLE_VALUE because it's a not a normal handle
        goto cleanup;
    }

    // Create trigger event for the worker thread.
    g_ConsoleEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if(!g_ConsoleEvent)
    {
        errcode = GetLastError();
        logf_gle("Error: CreateEvent failed");
        g_ConsoleEvent = INVALID_HANDLE_VALUE;
        goto cleanup;
    }

    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);

    // Start the worker thread.
    logf("Starting worker thread");
    workerHandle = CreateThread(NULL, 0, WorkerThread, cmdline, 0, NULL);
    if(!workerHandle)
    {
        errcode = GetLastError();
        logf_gle("Error: CreateThread failed");
        workerHandle = INVALID_HANDLE_VALUE;
        goto cleanup;
    }

    // Wait for the worker thread to exit.
    if(WaitForSingleObject(workerHandle, INFINITE) == WAIT_FAILED)
    {
        errcode = GetLastError();
        logf_gle("Error: WaitForSingleObject failed");
        goto cleanup;
    }

    if(!GetExitCodeThread(workerHandle, &errcode))
    {
        errcode = GetLastError();
        logf_gle("Error: GetExitCodeThread failed");
        goto cleanup;
    }

cleanup:
    logf("exiting");

    if(g_StatusHandle)
    {
        g_Status.dwCurrentState = SERVICE_STOPPED;
        g_Status.dwWin32ExitCode = errcode;
        SetServiceStatus(g_StatusHandle, &g_Status);
        // The service status handle is not a normal handle (it's a struct) and isn't closed.
    }

    if(g_ConsoleEvent != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_ConsoleEvent);
        g_ConsoleEvent = INVALID_HANDLE_VALUE;
    }

    if(workerHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(workerHandle);
        workerHandle = INVALID_HANDLE_VALUE;
    }

    logf("ServiceMain: end");
    return;
}

void SessionChange(DWORD eventType, WTSSESSION_NOTIFICATION *sn)
{
    if (eventType < RTL_NUMBER_OF(g_SessionEventName))
        logf("SessionChange: %s, session ID %I32u", g_SessionEventName[eventType], sn->dwSessionId);
    else
        logf("SessionChange: <unknown event: %I32u>, session id %I32u", eventType, sn->dwSessionId);

    if (eventType == WTS_CONSOLE_CONNECT)
    {
        // Store the new session ID for the worker thread and signal the trigger event.
        g_TargetSessionId = sn->dwSessionId;
        SetEvent(g_ConsoleEvent);
    }
}

DWORD WINAPI ControlHandlerEx(DWORD control, DWORD eventType, void *eventData, void *context)
{
    TCHAR *codename = GetControlCodeName(control);

    if(codename)
        logf("ControlHandlerEx: code %s, event 0x%I32x", codename, eventType);
    else
        logf("ControlHandlerEx: code 0x%I32x, event 0x%I32x", control, eventType);

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
    }

    return ERROR_SUCCESS;
}
