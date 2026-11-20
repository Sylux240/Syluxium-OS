/* The parts of the UEFI specification the boot picker uses (types,
 * tables and protocols, in specification order - layouts must match). */
#pragma once

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;
typedef long long i64;
typedef u64 UINTN;
typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef u16 CHAR16;
typedef u8 BOOLEAN;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS 0
#define EFI_ERR(x) (0x8000000000000000ULL | (x))
#define EFI_NOT_READY EFI_ERR(6)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)

typedef struct { u32 a; u16 b, c; u8 d[8]; } EFI_GUID;

typedef struct { u64 Signature; u32 Revision, HeaderSize, CRC32, Reserved; } EFI_TABLE_HEADER;

/* ---- console ---- */
typedef struct { u16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_INPUT {
    EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT *, BOOLEAN);
    EFI_STATUS(EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT;
typedef struct EFI_SIMPLE_TEXT_OUTPUT {
    void *Reset;
    EFI_STATUS(EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT *, const CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS(EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT *);
    void *SetCursorPosition;
    EFI_STATUS(EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT *, BOOLEAN);
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT;

/* ---- device paths ---- */
typedef struct { u8 Type, SubType, Length[2]; } EFI_DEVICE_PATH;

/* ---- boot services ---- */
typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;
#define EVT_TIMER 0x80000000
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    void *AllocatePages, *FreePages, *GetMemoryMap;
    EFI_STATUS(EFIAPI *AllocatePool)(u32 type, UINTN size, void **buffer);
    EFI_STATUS(EFIAPI *FreePool)(void *buffer);
    EFI_STATUS(EFIAPI *CreateEvent)(u32 type, UINTN tpl, void *fn, void *ctx, EFI_EVENT *event);
    EFI_STATUS(EFIAPI *SetTimer)(EFI_EVENT event, EFI_TIMER_DELAY type, u64 trigger_100ns);
    EFI_STATUS(EFIAPI *WaitForEvent)(UINTN n, EFI_EVENT *events, UINTN *index);
    void *SignalEvent;
    EFI_STATUS(EFIAPI *CloseEvent)(EFI_EVENT event);
    void *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS(EFIAPI *HandleProtocol)(EFI_HANDLE h, const EFI_GUID *guid, void **iface);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    EFI_STATUS(EFIAPI *LoadImage)(BOOLEAN boot_policy, EFI_HANDLE parent, EFI_DEVICE_PATH *path, void *src,
                                  UINTN src_size, EFI_HANDLE *image);
    EFI_STATUS(EFIAPI *StartImage)(EFI_HANDLE image, UINTN *exit_size, CHAR16 **exit_data);
    void *Exit, *UnloadImage, *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS(EFIAPI *Stall)(UINTN microseconds);
    EFI_STATUS(EFIAPI *SetWatchdogTimer)(UINTN timeout, u64 code, UINTN size, CHAR16 *data);
    EFI_STATUS(EFIAPI *ConnectController)(EFI_HANDLE controller, EFI_HANDLE *drivers, EFI_DEVICE_PATH *remaining, BOOLEAN recursive);
    void *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS(EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE type, const EFI_GUID *guid, void *key,
                                           UINTN *count, EFI_HANDLE **buffer);
    EFI_STATUS(EFIAPI *LocateProtocol)(const EFI_GUID *guid, void *registration, void **iface);
    void *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

/* ---- runtime services ---- */
typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown } EFI_RESET_TYPE;
#define EFI_VARIABLE_NON_VOLATILE 0x1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x2
#define EFI_VARIABLE_RUNTIME_ACCESS 0x4
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS(EFIAPI *GetVariable)(const CHAR16 *name, const EFI_GUID *guid, u32 *attrs, UINTN *size, void *data);
    void *GetNextVariableName;
    EFI_STATUS(EFIAPI *SetVariable)(const CHAR16 *name, const EFI_GUID *guid, u32 attrs, UINTN size, const void *data);
    void *GetNextHighMonotonicCount;
    void(EFIAPI *ResetSystem)(EFI_RESET_TYPE type, EFI_STATUS status, UINTN size, void *data);
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    u32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- graphics ---- */
typedef struct {
    u32 Version, HorizontalResolution, VerticalResolution, PixelFormat;
    u32 PixelBitmask[4];
    u32 PixelsPerScanLine;
} EFI_GOP_MODE_INFO;
typedef struct {
    u32 MaxMode, Mode;
    EFI_GOP_MODE_INFO *Info;
    UINTN SizeOfInfo;
    u64 FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GOP_MODE;
typedef enum { EfiBltVideoFill, EfiBltVideoToBltBuffer, EfiBltBufferToVideo, EfiBltVideoToVideo } EFI_BLT_OP;
typedef struct EFI_GOP {
    EFI_STATUS(EFIAPI *QueryMode)(struct EFI_GOP *, u32 mode, UINTN *size, EFI_GOP_MODE_INFO **info);
    EFI_STATUS(EFIAPI *SetMode)(struct EFI_GOP *, u32 mode);
    EFI_STATUS(EFIAPI *Blt)(struct EFI_GOP *, u32 *buffer, EFI_BLT_OP op, UINTN sx, UINTN sy, UINTN dx, UINTN dy,
                            UINTN w, UINTN h, UINTN delta);
    EFI_GOP_MODE *Mode;
} EFI_GOP;

/* ---- pointers ---- */
typedef struct { i32 RelativeMovementX, RelativeMovementY, RelativeMovementZ; BOOLEAN LeftButton, RightButton; } EFI_SIMPLE_POINTER_STATE;
typedef struct { u64 ResolutionX, ResolutionY, ResolutionZ; BOOLEAN LeftButton, RightButton; } EFI_SIMPLE_POINTER_MODE;
typedef struct EFI_SIMPLE_POINTER {
    EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_POINTER *, BOOLEAN);
    EFI_STATUS(EFIAPI *GetState)(struct EFI_SIMPLE_POINTER *, EFI_SIMPLE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_SIMPLE_POINTER_MODE *Mode;
} EFI_SIMPLE_POINTER;
typedef struct { u64 CurrentX, CurrentY, CurrentZ; u32 ActiveButtons; } EFI_ABSOLUTE_POINTER_STATE;
typedef struct { u64 AbsoluteMinX, AbsoluteMinY, AbsoluteMinZ, AbsoluteMaxX, AbsoluteMaxY, AbsoluteMaxZ; u32 Attributes; } EFI_ABSOLUTE_POINTER_MODE;
typedef struct EFI_ABSOLUTE_POINTER {
    EFI_STATUS(EFIAPI *Reset)(struct EFI_ABSOLUTE_POINTER *, BOOLEAN);
    EFI_STATUS(EFIAPI *GetState)(struct EFI_ABSOLUTE_POINTER *, EFI_ABSOLUTE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_ABSOLUTE_POINTER_MODE *Mode;
} EFI_ABSOLUTE_POINTER;

/* ---- images and files ---- */
typedef struct {
    u32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH *FilePath;
    void *Reserved;
    u32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    u64 ImageSize;
    u32 ImageCodeType, ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE;

#define EFI_FILE_MODE_READ 0x1ULL
#define EFI_FILE_DIRECTORY 0x10ULL
typedef struct EFI_FILE {
    u64 Revision;
    EFI_STATUS(EFIAPI *Open)(struct EFI_FILE *, struct EFI_FILE **, const CHAR16 *name, u64 mode, u64 attrs);
    EFI_STATUS(EFIAPI *Close)(struct EFI_FILE *);
    void *Delete;
    EFI_STATUS(EFIAPI *Read)(struct EFI_FILE *, UINTN *size, void *buffer);
    void *Write, *GetPosition, *SetPosition, *GetInfo, *SetInfo, *Flush;
} EFI_FILE;
typedef struct EFI_SIMPLE_FILE_SYSTEM {
    u64 Revision;
    EFI_STATUS(EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM *, EFI_FILE **root);
} EFI_SIMPLE_FILE_SYSTEM;
typedef struct { u16 Year; u8 Month, Day, Hour, Minute, Second, Pad1; u32 Nanosecond; i32 TimeZone_Daylight_Pad2; } EFI_TIME;
typedef struct {
    u64 Size, FileSize, PhysicalSize;
    EFI_TIME CreateTime, LastAccessTime, ModificationTime;
    u64 Attribute;
    CHAR16 FileName[];
} EFI_FILE_INFO;
