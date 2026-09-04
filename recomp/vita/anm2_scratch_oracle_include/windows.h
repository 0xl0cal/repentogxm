#ifndef ISAAC_ANM2_ORACLE_WINDOWS_H
#define ISAAC_ANM2_ORACLE_WINDOWS_H

/* Compile-only Win32 surface for the Linux host routing oracle.  The link uses
 * -ffunction-sections/--gc-sections and retains only guest.c's real stack
 * validators; no function declared here is implemented or executed. */
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef void *HANDLE;
typedef void *HMODULE;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef uint32_t DWORD;
typedef int BOOL;
typedef int32_t LONG;
typedef int64_t LONG64;
typedef struct _LARGE_INTEGER {
    int64_t QuadPart;
} LARGE_INTEGER;
typedef struct _MEMORY_BASIC_INFORMATION {
    void *BaseAddress;
    void *AllocationBase;
    DWORD AllocationProtect;
    size_t RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION;

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_SUCCESS 0U
#define ERROR_ENVVAR_NOT_FOUND 203U
#define GENERIC_READ 0x80000000U
#define GENERIC_WRITE 0x40000000U
#define FILE_SHARE_READ 0x00000001U
#define FILE_SHARE_WRITE 0x00000002U
#define OPEN_EXISTING 3U
#define FILE_ATTRIBUTE_NORMAL 0x00000080U
#define PAGE_NOACCESS 0x00000001U
#define PAGE_READWRITE 0x00000004U
#define PAGE_EXECUTE_READWRITE 0x00000040U
#define FILE_MAP_WRITE 0x00000002U
#define MEM_COMMIT 0x00001000U
#define MEM_RESERVE 0x00002000U
#define MEM_DECOMMIT 0x00004000U
#define MEM_RELEASE 0x00008000U
#define MEM_FREE 0x00010000U
#define _TRUNCATE ((size_t)-1)

BOOL CloseHandle(HANDLE object);
HANDLE CreateFileA(const char *name, DWORD access, DWORD share,
                   void *security, DWORD creation, DWORD attributes,
                   HANDLE template_file);
HANDLE CreateFileMappingA(HANDLE file, void *security, DWORD protection,
                          DWORD maximum_size_high, DWORD maximum_size_low,
                          const char *name);
BOOL FlushViewOfFile(const void *base, size_t bytes);
DWORD GetCurrentProcessId(void);
DWORD GetCurrentThreadId(void);
DWORD GetEnvironmentVariableA(const char *name, char *buffer, DWORD size);
BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size);
DWORD GetLastError(void);
HMODULE GetModuleHandleW(const wchar_t *name);
LONG64 InterlockedCompareExchange64(volatile LONG64 *destination,
                                    LONG64 exchange, LONG64 comparand);
LONG InterlockedExchange(volatile LONG *destination, LONG value);
int _snprintf_s(char *buffer, size_t buffer_size, size_t count,
                const char *format, ...);
void *MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high,
                    DWORD offset_low, size_t bytes);
void SetLastError(DWORD error);
BOOL UnmapViewOfFile(const void *base);
void *VirtualAlloc(void *address, size_t size, DWORD allocation_type,
                   DWORD protection);
BOOL VirtualFree(void *address, size_t size, DWORD free_type);
size_t VirtualQuery(const void *address, MEMORY_BASIC_INFORMATION *information,
                    size_t information_size);

#endif
