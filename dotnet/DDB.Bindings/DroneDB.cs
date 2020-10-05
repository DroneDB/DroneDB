using Newtonsoft.Json;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace DDB.Bindings
{
    public static class DroneDB
    {
        [DllImport("ddb", EntryPoint = "DDBRegisterProcess")]
        public static extern void RegisterProcess(bool verbose = false);

        [DllImport("ddb", EntryPoint = "DDBGetVersion")]
        private static extern IntPtr _GetVersion();

        public static string GetVersion()
        {
            var ptr = _GetVersion();
            return Marshal.PtrToStringAnsi(ptr);
        }

        [DllImport("ddb", EntryPoint = "DDBGetLastError")]
        private static extern IntPtr _GetLastError();

        static string GetLastError()
        {
            var ptr = _GetLastError();
            return Marshal.PtrToStringAnsi(ptr);
        }

        [DllImport("ddb", EntryPoint = "DDBInit")]
        private static extern DDBError _Init([MarshalAs(UnmanagedType.LPStr)]string directory, out IntPtr outPath);
        
        public static string Init(string directory)
        {
            if (_Init(directory, out var outPath) == DDBError.DDBERR_NONE)
                return Marshal.PtrToStringAnsi(outPath);
            
            throw new DDBException(GetLastError());
        }

        [DllImport("ddb", EntryPoint = "DDBAdd")]
        static extern DDBError _Add([MarshalAs(UnmanagedType.LPStr)] string ddbPath, 
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType=UnmanagedType.LPStr)] string[] paths, 
                                  int numPaths, out IntPtr output, bool recursive);

        public static List<Entry> Add(string ddbPath, string path, bool recursive = false)
        {
            return Add(ddbPath, new[] { path }, recursive);
        }
        public static List<Entry> Add(string ddbPath, string[] paths, bool recursive = false)
        {
            if (_Add(ddbPath, paths, paths.Length, out var output, recursive) != DDBError.DDBERR_NONE)
                throw new DDBException(GetLastError());

            var json = Marshal.PtrToStringAnsi(output);

            if (json == null)
                throw new DDBException("Unable to add");

            return JsonConvert.DeserializeObject<List<Entry>>(json);
        }

        [DllImport("ddb", EntryPoint = "DDBRemove")]
        static extern DDBError _Remove([MarshalAs(UnmanagedType.LPStr)] string ddbPath,
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                  int numPaths);

        public static void Remove(string ddbPath, string path)
        {
            Remove(ddbPath, new[] { path });
        }
        public static void Remove(string ddbPath, string[] paths)
        {
            if (_Remove(ddbPath, paths, paths.Length) != DDBError.DDBERR_NONE)
                throw new DDBException(GetLastError());
            
        }

        [DllImport("ddb", EntryPoint = "DDBInfo")]
        static extern DDBError _Info([MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                   int numPaths,
                                   out IntPtr output,
                                   [MarshalAs(UnmanagedType.LPStr)] string format, bool recursive = false,
                                   int maxRecursionDepth = 0, [MarshalAs(UnmanagedType.LPStr)] string geometry = "auto",
                                   bool withHash = false, bool stopOnError = true);

        public static List<Entry> Info(string path, bool recursive = false, int maxRecursionDepth = 0, bool withHash = false)
        {
            return Info(new[] { path }, recursive, maxRecursionDepth, withHash);
        }

        public static List<Entry> Info(string[] paths, bool recursive = false, int maxRecursionDepth = 0, bool withHash = false)
        {
            if (_Info(paths, paths.Length, out var output, "json", recursive, maxRecursionDepth, "auto", withHash) !=
                DDBError.DDBERR_NONE) throw new DDBException(GetLastError());

            var json = Marshal.PtrToStringAnsi(output);

            if (json == null)
                throw new DDBException("Unable get info");

            return JsonConvert.DeserializeObject<List<Entry>>(json); 

        }

        [DllImport("ddb", EntryPoint = "DDBList")]
        static extern DDBError _List([MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                   int numPaths,
                                   out IntPtr output,
                                   [MarshalAs(UnmanagedType.LPStr)] string format, 
                                   int maxRecursionDepth = 0);

        public static List<Entry> List(string[] paths, int maxRecursionDepth = 0)
        {
            if (_List(paths, paths.Length, out var output, "json", maxRecursionDepth) !=
                DDBError.DDBERR_NONE) throw new DDBException(GetLastError());

            var json = Marshal.PtrToStringAnsi(output);

            if (json == null)
                throw new DDBException("Unable get list");

            return JsonConvert.DeserializeObject<List<Entry>>(json); 

        }
    }
}
