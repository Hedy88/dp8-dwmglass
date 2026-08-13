// injector.cpp - DLL injector for dp8-dwmglass testing
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

bool IsElevated() {
  HANDLE hToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    return false;

  TOKEN_ELEVATION elevation = {0};
  DWORD size = 0;
  const bool ok = GetTokenInformation(hToken, TokenElevation, &elevation,
                                      sizeof(elevation), &size) &&
                  elevation.TokenIsElevated;

  CloseHandle(hToken);
  return ok;
}

bool EnableDebugPrivilege() {
  HANDLE hToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
    printf("Injector: OpenProcessToken failed (error %d)\n", GetLastError());
    return false;
  }

  TOKEN_PRIVILEGES tp = {0};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege",
                             &tp.Privileges[0].Luid)) {
    printf(
        "Injector: LookupPrivilegeValue(SeDebugPrivilege) failed (error %d)\n",
        GetLastError());
    CloseHandle(hToken);
    return false;
  }

  const bool ok =
      AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  const DWORD err = GetLastError();
  CloseHandle(hToken);

  if (!ok || err != ERROR_SUCCESS) {
    printf(
        "Injector: AdjustTokenPrivileges(SeDebugPrivilege) failed (error %d)\n",
        err);
    return false;
  }

  printf("Injector: SeDebugPrivilege enabled\n");
  return true;
}

struct RemoteParam {
  FARPROC fnLoadLibraryW;
  FARPROC fnGetLastError;
  LPCWSTR path;
  DWORD error_out;
};

// x64 stub executed in the target process:
//   rcx = RemoteParam* ; returns HMODULE (or 0) and stores GetLastError in
//   error_out
static const BYTE g_Stub[] = {
    0x48, 0x83, 0xEC, 0x28,       // sub rsp, 0x28
    0x48, 0x89, 0x4C, 0x24, 0x20, // mov [rsp+0x20], rcx
    0x48, 0x8B, 0x01,             // mov rax, [rcx+0x00]  (fnLoadLibraryW)
    0x48, 0x8B, 0x49, 0x10,       // mov rcx, [rcx+0x10]  (path)
    0xFF, 0xD0,                   // call rax
    0x48, 0x8B, 0x54, 0x24, 0x20, // mov rdx, [rsp+0x20]  (param)
    0xC7, 0x42, 0x18, 0x00, 0x00, 0x00, 0x00, // mov dword ptr [rdx+0x18], 0
    0x48, 0x85, 0xC0,                         // test rax, rax
    0x75, 0x11,                               // jnz done
    0x48, 0x8B, 0x42, 0x08,       // mov rax, [rdx+0x08]  (fnGetLastError)
    0xFF, 0xD0,                   // call rax
    0x48, 0x8B, 0x4C, 0x24, 0x20, // mov rcx, [rsp+0x20]
    0x48, 0x89, 0x41, 0x18,       // mov [rcx+0x18], rax  (error_out)
    0x31, 0xC0,                   // xor eax, eax
    0x48, 0x83, 0xC4, 0x28,       // done: add rsp, 0x28
    0xC3                          // ret
};

void LogRemoteError(DWORD code) {
  const wchar_t *name = L"";
  switch (code) {
  case ERROR_FILE_NOT_FOUND:
    name = L"ERROR_FILE_NOT_FOUND";
    break;
  case ERROR_PATH_NOT_FOUND:
    name = L"ERROR_PATH_NOT_FOUND";
    break;
  case ERROR_ACCESS_DENIED:
    name = L"ERROR_ACCESS_DENIED";
    break;
  case ERROR_NOT_ENOUGH_MEMORY:
    name = L"ERROR_NOT_ENOUGH_MEMORY";
    break;
  case ERROR_MOD_NOT_FOUND:
    name = L"ERROR_MOD_NOT_FOUND (missing dependency?)";
    break;
  case ERROR_PROC_NOT_FOUND:
    name = L"ERROR_PROC_NOT_FOUND";
    break;
  case ERROR_BAD_EXE_FORMAT:
    name = L"ERROR_BAD_EXE_FORMAT (wrong architecture?)";
    break;
  case ERROR_NOACCESS:
    name = L"ERROR_NOACCESS";
    break;
  }

  if (name[0] != L'\0')
    printf("Injector: remote LoadLibraryW failed, GetLastError=0x%lx (%ls)\n",
           code, name);
  else
    printf("Injector: remote LoadLibraryW failed, GetLastError=0x%lx\n", code);
}

// Distinguishes "DLL never mapped into the target" (failure in early image
// processing) from "DLL mapped but initialization crashed" (module present).
bool ModuleLoadedIn(DWORD pid, const wchar_t *base_name) {
  HANDLE hProcess =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!hProcess) {
    printf("Injector: ModuleLoadedIn: OpenProcess failed (error %d)\n",
           GetLastError());
    return false;
  }

  HMODULE mods[512] = {0};
  DWORD needed = 0;
  bool found = false;
  if (EnumProcessModules(hProcess, mods, sizeof(mods), &needed)) {
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; i++) {
      wchar_t name[MAX_PATH] = {0};
      if (GetModuleBaseNameW(hProcess, mods[i], name, ARRAYSIZE(name)) &&
          _wcsicmp(name, base_name) == 0) {
        found = true;
        break;
      }
    }
  } else {
    printf("Injector: ModuleLoadedIn: EnumProcessModules failed (error %d)\n",
           GetLastError());
  }

  CloseHandle(hProcess);
  return found;
}

DWORD FindProcessId(const wchar_t *process_name) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    printf("Injector: CreateToolhelp32Snapshot failed\n");
    return 0;
  }

  PROCESSENTRY32W entry = {0};
  entry.dwSize = sizeof(entry);

  if (!Process32FirstW(snapshot, &entry)) {
    printf("Injector: Process32FirstW failed\n");
    CloseHandle(snapshot);
    return 0;
  }

  DWORD pid = 0;
  do {
    if (_wcsicmp(entry.szExeFile, process_name) == 0) {
      pid = entry.th32ProcessID;
      break;
    }
  } while (Process32NextW(snapshot, &entry));

  CloseHandle(snapshot);
  return pid;
}

bool InjectDLL(DWORD pid, const wchar_t *dll_path) {
  printf("Injector: Injecting %ls into PID %d\n", dll_path, pid);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                      PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                  FALSE, pid);
  if (!hProcess) {
    printf("Injector: OpenProcess failed (error %d)\n", GetLastError());
    return false;
  }

  const SIZE_T path_size = (wcslen(dll_path) + 1) * sizeof(wchar_t);
  const SIZE_T param_size = sizeof(RemoteParam);
  const SIZE_T total_size = param_size + sizeof(g_Stub) + path_size;

  // Remote buffer layout: [RemoteParam][stub][wide path]
  LPVOID remote_base =
      VirtualAllocEx(hProcess, nullptr, total_size, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
  if (!remote_base) {
    printf("Injector: VirtualAllocEx failed (error %d)\n", GetLastError());
    CloseHandle(hProcess);
    return false;
  }

  BYTE *remote_param = static_cast<BYTE *>(remote_base);
  BYTE *remote_stub = remote_param + param_size;
  BYTE *remote_path = remote_stub + sizeof(g_Stub);

  // Write the DLL path first
  if (!WriteProcessMemory(hProcess, remote_path, dll_path, path_size,
                          nullptr)) {
    printf("Injector: WriteProcessMemory(path) failed (error %d)\n",
           GetLastError());
    VirtualFreeEx(hProcess, remote_base, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    return false;
  }

  RemoteParam param = {};
  param.fnLoadLibraryW =
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
  param.fnGetLastError =
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLastError");
  param.path = reinterpret_cast<LPCWSTR>(remote_path);
  param.error_out = 0;

  if (!param.fnLoadLibraryW || !param.fnGetLastError) {
    printf("Injector: GetProcAddress on kernel32 failed (error %d)\n",
           GetLastError());
    VirtualFreeEx(hProcess, remote_base, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    return false;
  }

  if (!WriteProcessMemory(hProcess, remote_param, &param, sizeof(param),
                          nullptr)) {
    printf("Injector: WriteProcessMemory(param) failed (error %d)\n",
           GetLastError());
    VirtualFreeEx(hProcess, remote_base, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    return false;
  }

  if (!WriteProcessMemory(hProcess, remote_stub, g_Stub, sizeof(g_Stub),
                          nullptr)) {
    printf("Injector: WriteProcessMemory(stub) failed (error %d)\n",
           GetLastError());
    VirtualFreeEx(hProcess, remote_base, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    return false;
  }

  printf("Injector: Remote thread created, waiting...\n");

  HANDLE hThread =
      CreateRemoteThread(hProcess, nullptr, 0,
                         reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
                         remote_param, 0, nullptr);
  if (!hThread) {
    printf("Injector: CreateRemoteThread failed (error %d)\n", GetLastError());
    VirtualFreeEx(hProcess, remote_base, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    return false;
  }

  WaitForSingleObject(hThread, INFINITE);

  // Read back the captured result and error code
  DWORD exit_code = 0;
  DWORD remote_error = 0;
  GetExitCodeThread(hThread, &exit_code);
  ReadProcessMemory(hProcess,
                    &reinterpret_cast<RemoteParam *>(remote_param)->error_out,
                    &remote_error, sizeof(remote_error), nullptr);

  CloseHandle(hThread);

  if (exit_code == 0) {
    printf("Injector: DLL returned 0 (possible failure)\n");
    LogRemoteError(remote_error);
    const wchar_t *base = wcsrchr(dll_path, L'\\');
    base = base ? base + 1 : dll_path;
    if (ModuleLoadedIn(pid, base))
      printf("Injector: %ls IS mapped in target (crashed after mapping)\n",
             base);
    else
      printf(
          "Injector: %ls NOT mapped in target (failed before/at mapping)\n",
          base);
  } else {
    printf("Injector: DLL loaded successfully (HMODULE=0x%08lx)\n", exit_code);
  }

  VirtualFreeEx(hProcess, remote_base, 0, MEM_RELEASE);
  CloseHandle(hProcess);

  return exit_code != 0;
}

bool InjectIntoDWM(const wchar_t *dll_path, const wchar_t *process_name) {
  // First try to find existing process
  DWORD pid = FindProcessId(process_name);
  if (pid == 0) {
    printf("Injector: %ls not running\n", process_name);
    return false;
  }

  return InjectDLL(pid, dll_path);
}

int wmain(int argc, wchar_t *argv[]) {
  if (argc < 2) {
    printf("Usage: injector.exe <dll_path> [process_name]\n");
    printf("Example: injector.exe dp8-dwmglass.dll dwm.exe\n");
    return 1;
  }

  if (!IsElevated()) {
    printf("Injector: NOT elevated - run from an elevated (Administrator) "
           "console\n");
    printf("Injection into dwm.exe will fail with error 5 otherwise\n");
    return 1;
  }

  EnableDebugPrivilege();

  const wchar_t *dll_path = argv[1];
  const wchar_t *process_name = (argc >= 3) ? argv[2] : L"dwm.exe";

  if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES) {
    printf("Injector: DLL not found: %ls\n", dll_path);
    return 1;
  }

  // Resolve to an absolute path: the path is used by LoadLibraryW in the
  // TARGET process, whose current directory is NOT the injector's.
  wchar_t dll_full[MAX_PATH] = {0};
  const DWORD path_len =
      GetFullPathNameW(dll_path, ARRAYSIZE(dll_full), dll_full, nullptr);
  if (path_len == 0 || path_len >= ARRAYSIZE(dll_full)) {
    printf("Injector: GetFullPathNameW failed (error %d)\n", GetLastError());
    return 1;
  }
  dll_path = dll_full;
  printf("Injector: Resolved DLL path: %ls\n", dll_path);

  if (!InjectIntoDWM(dll_path, process_name)) {
    printf("Injector: Injection failed\n");
    return 1;
  }

  printf("Injector: Injection complete\n");
  return 0;
}
