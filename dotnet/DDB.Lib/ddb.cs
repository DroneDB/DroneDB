using System;
using System.Runtime.InteropServices;

namespace DDB.Lib
{
    enum DDBErr
    {
        DDBERR_NONE = 0, // No error
        DDBERR_EXCEPTION = 1 // Generic app exception
    };

    public static class Exports
    {
        [DllImport("ddb", EntryPoint = "DDBRegisterProcess")]
        public static extern void RegisterProcess(bool verbose = false);

        [DllImport("ddb", EntryPoint = "DDBGetVersion")]
        static extern IntPtr _GetVersion();

        public static string GetVersion()
        {
            var ptr = _GetVersion();
            return Marshal.PtrToStringAnsi(ptr);
        }

        [DllImport("ddb", EntryPoint = "DDBGetLastError")]
        static extern IntPtr _GetLastError();

        static string GetLastError()
        {
            var ptr = _GetLastError();
            return Marshal.PtrToStringAnsi(ptr);
        }

        [DllImport("ddb", EntryPoint = "DDBInit")]
        static extern DDBErr _Init([MarshalAs(UnmanagedType.LPStr)]string directory, out IntPtr outPath);
        
        public static string Init(string directory)
        {
            IntPtr outPath;
            if (_Init(directory, out outPath) == DDBErr.DDBERR_NONE)
            {
                return Marshal.PtrToStringAnsi(outPath);
            }
            else
            {
                throw new DDBException(GetLastError());
            }
        }
    }
}
