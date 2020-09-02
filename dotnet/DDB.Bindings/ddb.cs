using Newtonsoft.Json;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace DDB.Bindings
{
    enum DDBErr
    {
        DDBERR_NONE = 0, // No error
        DDBERR_EXCEPTION = 1 // Generic app exception
    };

    public static class Exports
    {
        [DllImport("./ddb", EntryPoint = "DDBRegisterProcess")]
        public static extern void RegisterProcess(bool verbose = false);

        [DllImport("./ddb", EntryPoint = "DDBGetVersion")]
        static extern IntPtr _GetVersion();

        public static string GetVersion()
        {
            var ptr = _GetVersion();
            return Marshal.PtrToStringAnsi(ptr);
        }

        [DllImport("./ddb", EntryPoint = "DDBGetLastError")]
        static extern IntPtr _GetLastError();

        static string GetLastError()
        {
            var ptr = _GetLastError();
            return Marshal.PtrToStringAnsi(ptr);
        }

        [DllImport("./ddb", EntryPoint = "DDBInit")]
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

        [DllImport("./ddb", EntryPoint = "DDBAdd")]
        static extern DDBErr _Add([MarshalAs(UnmanagedType.LPStr)] string ddbPath, 
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType=UnmanagedType.LPStr)] string[] paths, 
                                  int numPaths, bool recursive);

        public static void Add(string ddbPath, string path, bool recursive = false)
        {
            Add(ddbPath, new string[] { path }, recursive);
        }
        public static void Add(string ddbPath, string[] paths, bool recursive = false)
        {
            if (_Add(ddbPath, paths, paths.Length, recursive) != DDBErr.DDBERR_NONE) 
            { 
                throw new DDBException(GetLastError());
            }
        }

        [DllImport("./ddb", EntryPoint = "DDBRemove")]
        static extern DDBErr _Remove([MarshalAs(UnmanagedType.LPStr)] string ddbPath,
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                  int numPaths, bool recursive);

        public static void Remove(string ddbPath, string path, bool recursive = false)
        {
            Remove(ddbPath, new string[] { path }, recursive);
        }
        public static void Remove(string ddbPath, string[] paths, bool recursive = false)
        {
            if (_Remove(ddbPath, paths, paths.Length, recursive) != DDBErr.DDBERR_NONE)
            {
                throw new DDBException(GetLastError());
            }
        }

        [DllImport("./ddb", EntryPoint = "DDBInfo")]
        static extern DDBErr _Info([MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                   int numPaths,
                                   out IntPtr output,
                                   [MarshalAs(UnmanagedType.LPStr)] string format, bool recursive = false,
                                   int maxRecursionDepth = 0, [MarshalAs(UnmanagedType.LPStr)] string geometry = "auto",
                                   bool withHash = false, bool stopOnError = true);

        public static Entry Info(string path, bool recursive = false, int maxRecursionDepth = 0, bool withHash = false)
        {
            return Info(new string[] { path }, recursive, maxRecursionDepth, withHash).First();
        }

        public static List<Entry> Info(string[] paths, bool recursive = false, int maxRecursionDepth = 0, bool withHash = false)
        {
            IntPtr output;
            if (_Info(paths, paths.Length, out output, "json", recursive, maxRecursionDepth, "auto", withHash, true) == DDBErr.DDBERR_NONE)
            {
                string json = Marshal.PtrToStringAnsi(output);
                List<Entry> entries = JsonConvert.DeserializeObject<List<Entry>>(json);
                return entries;
            }
            else
            {
                throw new DDBException(GetLastError());
            }
        }
    }
}
