$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CZDesktopProbeNative {
    const uint DesktopReadObjects = 0x0001;
    const uint UserObjectName = 2;
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr GetThreadDesktop(uint threadId);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError=true)] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool GetUserObjectInformationW(IntPtr handle, uint index, IntPtr data, uint length, out uint needed);
    static string Name(IntPtr handle) {
        if (handle == IntPtr.Zero || handle == new IntPtr(-1)) return null;
        GetUserObjectInformationW(handle, UserObjectName, IntPtr.Zero, 0, out uint needed);
        if (Marshal.GetLastWin32Error() != 122 || needed == 0) return null;
        IntPtr buffer = Marshal.AllocHGlobal((int)needed);
        try {
            if (!GetUserObjectInformationW(handle, UserObjectName, buffer, needed, out needed)) return null;
            return Marshal.PtrToStringUni(buffer);
        } finally { Marshal.FreeHGlobal(buffer); }
    }
    public static string CurrentName() => Name(GetThreadDesktop(GetCurrentThreadId()));
    public static string InputName() {
        IntPtr desktop = OpenInputDesktop(0, false, DesktopReadObjects);
        if (desktop == IntPtr.Zero || desktop == new IntPtr(-1)) return null;
        try { return Name(desktop); } finally { CloseDesktop(desktop); }
    }
}
'@

$expected = $env:CZ_EDITOR_SMOKE_ISOLATED_DESKTOP
$current = [CZDesktopProbeNative]::CurrentName()
$input = [CZDesktopProbeNative]::InputName()
$result = [ordered]@{
    processId = $PID
    expected = $expected
    current = $current
    input = $input
    currentMatchesExpected = ($current -eq $expected)
    currentDiffersFromInput = ($current -ne $input)
}
$result | ConvertTo-Json -Compress
if (-not $result.currentMatchesExpected -or -not $result.currentDiffersFromInput) { exit 42 }
exit 0
