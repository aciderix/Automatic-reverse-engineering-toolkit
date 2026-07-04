//! Stack-pop counts for `__stdcall` Win32 imports (the callee-pops-args ABI).
//!
//! A 32-bit `__stdcall` callee removes its own arguments from the stack with
//! `ret N`. The transpiler's import shims read arguments off the modelled stack
//! but never pop them, so without correction `esp` ends `N` bytes too low after
//! every stdcall import call — corrupting later stack-relative accesses (a real
//! BusyBox bug: `SetLastError` left `esp` off by 4, so a spilled `fd` was reloaded
//! from the wrong slot and writes went to fd 0).
//!
//! The pop count is the `@N` decoration mingw's import libraries attach to these
//! symbols (`__imp__SetLastError@4`). That decoration is stripped from the PE
//! import table (only `SetLastError` survives), but `@N` is a fixed property of
//! each Win32 API — identical in every binary — so it is encoded here once. The
//! list covers the kernel32/user32/advapi32/ws2_32 stdcall functions seen in
//! practice; it is purely additive (an unlisted import just gets no pop modelled,
//! the prior behaviour) and reusable across binaries.

/// Bytes a `__stdcall` import pops on return (`@N`), or `None` if unknown/cdecl.
/// `name` is the raw PE import name (any single leading underscore is ignored).
pub fn stdcall_pop_bytes(name: &str) -> Option<u32> {
    let n = name.strip_prefix('_').unwrap_or(name);
    TABLE.binary_search_by(|&(k, _)| k.cmp(n)).ok().map(|i| TABLE[i].1)
}

/// (name, pop-bytes), sorted by name for binary search. `@0` functions are
/// omitted (nothing to pop).
static TABLE: &[(&str, u32)] = &[
    ("AccessCheck", 32),
    ("CheckTokenMembership", 12),
    ("CloseHandle", 4),
    ("CreateConsoleScreenBuffer", 20),
    ("CreateEventA", 16),
    ("CreateFileA", 28),
    ("CreateFileMappingA", 24),
    ("CreateNamedPipeA", 32),
    ("CreatePipe", 16),
    ("CreateProcessA", 40),
    ("CreateRemoteThread", 28),
    ("CreateToolhelp32Snapshot", 8),
    ("CryptAcquireContextA", 20),
    ("CryptGenRandom", 12),
    ("CryptReleaseContext", 8),
    ("DecodePointer", 4),
    ("DeleteCriticalSection", 4),
    ("DeviceIoControl", 32),
    ("DispatchMessageA", 4),
    ("DuplicateHandle", 28),
    ("DuplicateToken", 12),
    ("EncodePointer", 4),
    ("EnterCriticalSection", 4),
    ("EqualSid", 8),
    ("FileTimeToSystemTime", 8),
    ("FillConsoleOutputAttribute", 20),
    ("FillConsoleOutputCharacterA", 20),
    ("FindClose", 4),
    ("FindFirstFileA", 8),
    ("FindFirstVolumeA", 8),
    ("FindFirstVolumeW", 8),
    ("FindNextFileA", 8),
    ("FindNextVolumeA", 12),
    ("FindNextVolumeW", 12),
    ("FindVolumeClose", 4),
    ("FlushFileBuffers", 4),
    ("FormatMessageA", 28),
    ("FreeLibrary", 4),
    ("GenerateConsoleCtrlEvent", 8),
    ("GetCPInfo", 8),
    ("GetCompressedFileSizeA", 8),
    ("GetConsoleMode", 8),
    ("GetConsoleScreenBufferInfo", 8),
    ("GetConsoleTitleA", 8),
    ("GetDiskFreeSpaceExA", 16),
    ("GetDiskFreeSpaceExW", 16),
    ("GetDriveTypeA", 4),
    ("GetEnvironmentVariableW", 12),
    ("GetExitCodeProcess", 8),
    ("GetFileAttributesA", 4),
    ("GetFileAttributesExA", 12),
    ("GetFileInformationByHandle", 8),
    ("GetFileSize", 8),
    ("GetFileSizeEx", 8),
    ("GetFileType", 4),
    ("GetFileVersionInfoA", 16),
    ("GetFileVersionInfoSizeA", 8),
    ("GetFullPathNameA", 16),
    ("GetModuleFileNameA", 12),
    ("GetModuleHandleA", 4),
    ("GetModuleHandleW", 4),
    ("GetNumberOfConsoleInputEvents", 8),
    ("GetProcAddress", 8),
    ("GetProcessAffinityMask", 12),
    ("GetProcessId", 4),
    ("GetProcessTimes", 20),
    ("GetSecurityInfo", 32),
    ("GetStdHandle", 4),
    ("GetSystemDirectoryA", 8),
    ("GetSystemInfo", 4),
    ("GetSystemTimeAsFileTime", 4),
    ("GetTokenInformation", 20),
    ("GetUserNameA", 8),
    ("GetVersionExA", 4),
    ("GetVolumeInformationA", 32),
    ("GetVolumeInformationW", 32),
    ("GetVolumeNameForVolumeMountPointA", 12),
    ("GetVolumePathNamesForVolumeNameA", 16),
    ("InitializeCriticalSection", 4),
    ("InitializeCriticalSectionAndSpinCount", 8),
    ("InitializeCriticalSectionEx", 12),
    ("InitializeSListHead", 4),
    ("IsDBCSLeadByteEx", 8),
    ("IsIconic", 4),
    ("IsValidCodePage", 4),
    ("IsWindowVisible", 4),
    ("IsWow64Process", 8),
    ("LeaveCriticalSection", 4),
    ("LoadLibraryA", 4),
    ("LoadLibraryExA", 12),
    ("LocalFree", 4),
    ("LockFileEx", 24),
    ("MapViewOfFile", 20),
    ("MoveFileExA", 12),
    ("MsgWaitForMultipleObjects", 20),
    ("MultiByteToWideChar", 24),
    ("OpenProcess", 12),
    ("OpenProcessToken", 12),
    ("PeekConsoleInputW", 16),
    ("PeekMessageA", 20),
    ("PeekNamedPipe", 24),
    ("Process32First", 8),
    ("Process32Next", 8),
    ("ReadConsoleInputA", 16),
    ("ReadConsoleInputW", 16),
    ("ReadDirectoryChangesW", 32),
    ("ReadProcessMemory", 20),
    ("ResetEvent", 4),
    ("SaferComputeTokenFromLevel", 20),
    ("SaferCreateLevel", 20),
    ("SetConsoleActiveScreenBuffer", 4),
    ("SetConsoleCP", 4),
    ("SetConsoleCtrlHandler", 8),
    ("SetConsoleCursorPosition", 8),
    ("SetConsoleMode", 8),
    ("SetConsoleOutputCP", 4),
    ("SetConsoleScreenBufferSize", 8),
    ("SetConsoleTextAttribute", 8),
    ("SetConsoleTitleA", 4),
    ("SetEndOfFile", 4),
    ("SetEnvironmentVariableA", 8),
    ("SetErrorMode", 4),
    ("SetFileAttributesA", 8),
    ("SetFilePointer", 16),
    ("SetFileTime", 16),
    ("SetHandleInformation", 12),
    ("SetLastError", 4),
    ("SetSystemTime", 4),
    ("SetTokenInformation", 16),
    ("SetUnhandledExceptionFilter", 4),
    ("ShowWindow", 8),
    ("Sleep", 4),
    ("SleepEx", 8),
    ("SystemFunction036", 8),
    ("TerminateProcess", 8),
    ("TlsGetValue", 4),
    ("TranslateMessage", 4),
    ("UnlockFile", 20),
    ("UnmapViewOfFile", 4),
    ("VerQueryValueA", 16),
    ("VirtualProtect", 16),
    ("VirtualQuery", 12),
    ("WSAEnumNetworkEvents", 12),
    ("WSAEventSelect", 12),
    ("WSASetLastError", 4),
    ("WSASocketA", 24),
    ("WSAStartup", 8),
    ("WaitForMultipleObjects", 16),
    ("WaitForSingleObject", 8),
    ("WideCharToMultiByte", 32),
    ("accept", 12),
    ("bind", 12),
    ("closesocket", 4),
    ("connect", 12),
    ("freeaddrinfo", 4),
    ("getaddrinfo", 16),
    ("gethostbyaddr", 12),
    ("gethostname", 8),
    ("getnameinfo", 28),
    ("getpeername", 12),
    ("listen", 8),
    ("recv", 16),
    ("select", 20),
    ("setsockopt", 20),
    ("shutdown", 8),
];

#[cfg(test)]
mod tests {
    use super::TABLE;

    /// `stdcall_pop_bytes` binary-searches `TABLE`, so an out-of-order entry
    /// silently makes some imports unfindable — their pop is skipped and every
    /// later stack access drifts. Guard the invariant here instead of trusting
    /// hand-maintained alphabetical order.
    #[test]
    fn table_is_sorted_by_name() {
        for pair in TABLE.windows(2) {
            assert!(
                pair[0].0 < pair[1].0,
                "stdcall_pops TABLE not sorted: {:?} !< {:?}",
                pair[0].0,
                pair[1].0
            );
        }
    }
}
