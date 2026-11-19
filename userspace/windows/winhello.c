// A minimal real Windows program for testing Wine (9-wine): built twice, as
// winhello64.exe (x86-64) and winhello32.exe (i386, runs through WoW64),
// with clang + lld-link and no C runtime. Prints a line on the console;
// with /m it also shows a message box (needs the desktop). winbox32.exe is
// a GUI-subsystem build that always shows the box (double-click tests).
//
//   wine /opt/syluxium/tests/winhello32.exe [/m]
typedef void* HANDLE;
typedef unsigned long DWORD;
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) char* __stdcall GetCommandLineA(void);
__declspec(dllimport) int __stdcall MessageBoxA(void*, const char*, const char*, unsigned);

static DWORD len(const char* s) {
    DWORD n = 0;
    while (s[n])
        n++;
    return n;
}

void __stdcall start(void) {
    HANDLE out = GetStdHandle((DWORD)-11); // STD_OUTPUT_HANDLE
    const char* msg = sizeof(void*) == 8 ? "Hello from a real 64-bit Windows program\r\n"
                                         : "Hello from a real 32-bit Windows program\r\n";
    DWORD n;
    WriteFile(out, msg, len(msg), &n, 0);
    for (const char* p = GetCommandLineA(); *p; p++) {
#ifdef ALWAYS_BOX
        if (1) {
#else
        if (p[0] == '/' && p[1] == 'm') {
#endif
            MessageBoxA(0, sizeof(void*) == 8 ? "A 64-bit Windows program running on Syluxium"
                                              : "A 32-bit Windows program running on Syluxium",
                        "Hello from Windows", 0);
            break;
        }
    }
    ExitProcess(0);
}
