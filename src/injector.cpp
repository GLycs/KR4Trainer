// ============================================================================
// KR4Mod - x64 injector
//   Loads an x64 DLL into a running process via CreateRemoteThread(LoadLibraryW).
//
// Usage:
//   kr4injector.exe --dll <path\to\kr4mod.dll> [--name <exe name>] [--pid <pid>] [--wait <seconds>]
//   kr4injector.exe <dll path> [exe name]
//
// Default target name: "Kingdom Rush Vengeance.exe"
// With --wait N, the injector polls for the target process for up to N seconds
// (useful right before pressing Play in Steam).
//
// Exit codes: 0 ok, 1 usage, 2 process not found, 3 open/alloc/write failed,
//             4 remote thread failed, 5 LoadLibrary returned null
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static DWORD FindPidByName(const char* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    DWORD pid = 0;
    if (Process32First(snap, &pe))
    {
        do
        {
            if (lstrcmpiA(pe.szExeFile, exeName) == 0)
            {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool InjectDll(DWORD pid, const char* dllPath)
{
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc)
    {
        printf("[!] OpenProcess(%lu) failed, err=%lu (try running as admin)\n",
               (unsigned long)pid, GetLastError());
        return false;
    }

    wchar_t wpath[MAX_PATH * 2] = {0};
    if (!MultiByteToWideChar(CP_ACP, 0, dllPath, -1, wpath, MAX_PATH * 2))
    {
        printf("[!] path conversion failed\n");
        CloseHandle(hProc);
        return false;
    }
    SIZE_T bytes = (wcslen(wpath) + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        printf("[!] VirtualAllocEx failed, err=%lu\n", GetLastError());
        CloseHandle(hProc);
        return false;
    }
    if (!WriteProcessMemory(hProc, remote, wpath, bytes, nullptr))
    {
        printf("[!] WriteProcessMemory failed, err=%lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HMODULE hKernel = GetModuleHandleA("kernel32.dll");
    void* pLoadLibraryW = (void*)GetProcAddress(hKernel, "LoadLibraryW");
    if (!pLoadLibraryW)
    {
        printf("[!] LoadLibraryW not resolved\n");
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
                                        (LPTHREAD_START_ROUTINE)pLoadLibraryW, remote, 0, nullptr);
    if (!hThread)
    {
        printf("[!] CreateRemoteThread failed, err=%lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    DWORD wait = WaitForSingleObject(hThread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (wait != WAIT_OBJECT_0)
    {
        printf("[!] remote thread did not finish in time\n");
        return false;
    }
    if (exitCode == 0)
    {
        printf("[!] LoadLibraryW returned NULL - DLL load failed in target\n");
        printf("    check bitness (must be x64) and that dependencies resolve\n");
        return false;
    }

    printf("[+] DLL loaded in pid %lu (remote HMODULE=0x%llx)\n",
           (unsigned long)pid, (unsigned long long)exitCode);
    return true;
}

int main(int argc, char** argv)
{
    const char* dllPath  = nullptr;
    const char* exeName  = "Kingdom Rush Vengeance.exe";
    DWORD       pid      = 0;
    int         waitSec  = 0;

    for (int i = 1; i < argc; ++i)
    {
        if (lstrcmpiA(argv[i], "--dll") == 0 && i + 1 < argc)        dllPath = argv[++i];
        else if (lstrcmpiA(argv[i], "--name") == 0 && i + 1 < argc)  exeName = argv[++i];
        else if (lstrcmpiA(argv[i], "--pid") == 0 && i + 1 < argc)   pid = (DWORD)strtoul(argv[++i], nullptr, 10);
        else if (lstrcmpiA(argv[i], "--wait") == 0 && i + 1 < argc)  waitSec = atoi(argv[++i]);
        else if (!dllPath)                                           dllPath = argv[i];
        else                                                         exeName = argv[i];
    }

    if (!dllPath)
    {
        printf("KR4Mod injector (x64)\n");
        printf("  kr4injector.exe --dll <kr4mod.dll> [--name <exe>] [--pid <pid>] [--wait <sec>]\n");
        printf("  kr4injector.exe <kr4mod.dll> [exe name]\n");
        return 1;
    }

    char full[MAX_PATH] = {0};
    if (!GetFullPathNameA(dllPath, MAX_PATH, full, nullptr))
    {
        printf("[!] bad dll path\n");
        return 1;
    }
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES)
    {
        printf("[!] dll not found: %s\n", full);
        return 1;
    }
    printf("[*] dll  : %s\n", full);

    if (pid == 0)
    {
        printf("[*] target: %s\n", exeName);
        int waited = 0;
        while ((pid = FindPidByName(exeName)) == 0)
        {
            if (waited >= waitSec)
            {
                printf("[!] process not found\n");
                return 2;
            }
            Sleep(500);
            waited++;
        }
    }
    printf("[*] pid  : %lu\n", (unsigned long)pid);

    return InjectDll(pid, full) ? 0 : 4;
}
