// pattern_scan.cpp - pattern scanner implementation

#include "pattern_scan.h"

#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

BYTE *PatternScan(const wchar_t *module_name, const wchar_t *pattern) {
  if (!module_name || !pattern) {
    printf("PatternScan: Invalid parameters\n");
    return nullptr;
  }

  HMODULE hModule = GetModuleHandleW(module_name);
  if (!hModule) {
    printf("PatternScan: GetModuleHandleW(%ls) failed\n", module_name);
    return nullptr;
  }

  MODULEINFO mi = {0};
  if (!K32GetModuleInformation(GetCurrentProcess(), hModule, &mi, sizeof(mi))) {
    printf("PatternScan: K32GetModuleInformation failed for %ls\n",
           module_name);
    return nullptr;
  }

  BYTE *base = (BYTE *)mi.lpBaseOfDll;
  SIZE_T size = mi.SizeOfImage;
  size_t patternLen = wcslen(pattern) * sizeof(wchar_t);

  if (size < patternLen) {
    printf("PatternScan: module too small\n");
    return nullptr;
  }

  SIZE_T scanRange = size - patternLen;
  BYTE *found = nullptr;
  int matchCount = 0;

  for (SIZE_T i = 0; i <= scanRange; i++) {
    bool match = true;
    for (size_t j = 0; j < patternLen; j += 2) {
      wchar_t pchar = *(wchar_t *)(pattern + j);
      if (pchar >= 0 && base[i + j] != (BYTE)pchar) {
        match = false;
        break;
      }
      if (pchar < 0 && base[i + j] != (BYTE)(pchar >> 8)) {
        match = false;
        break;
      }
    }
    if (match) {
      matchCount++;
      found = base + i;
    }
  }

  if (matchCount == 1) {
    printf("PatternScan: Found %ls in %ls at 0x%p\n", pattern, module_name,
           found);
    return found;
  }

  printf("PatternScan: %ls not found or ambiguous in %ls (%d matches)\n",
         pattern, module_name, matchCount);
  return nullptr;
}
