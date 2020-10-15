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
        private static extern DDBError _Init([MarshalAs(UnmanagedType.LPStr)] string directory, out IntPtr outPath);

        public static string Init(string directory)
        {
            if (string.IsNullOrWhiteSpace(directory))
                throw new DDBException("Directory should not be null or empty");

            if (_Init(directory, out var outPath) == DDBError.DDBERR_NONE)
                return Marshal.PtrToStringAnsi(outPath);

            throw new DDBException(GetLastError());
        }

        [DllImport("ddb", EntryPoint = "DDBAdd")]
        static extern DDBError _Add([MarshalAs(UnmanagedType.LPStr)] string ddbPath,
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                  int numPaths, out IntPtr output, bool recursive);

        public static List<Entry> Add(string ddbPath, string path, bool recursive = false)
        {

            if (string.IsNullOrWhiteSpace(path))
                throw new DDBException("Path should not be null or empty");
            
            return Add(ddbPath, new[] { path }, recursive);
        }
        public static List<Entry> Add(string ddbPath, string[] paths, bool recursive = false)
        {

            if (string.IsNullOrWhiteSpace(ddbPath))
                throw new DDBException("DDB path should not be null or empty");

            if (paths == null || paths.Any(string.IsNullOrWhiteSpace))
                throw new DDBException("One of the provided paths is null or empty");
            
            if (_Add(ddbPath, paths, paths.Length, out var output, recursive) != DDBError.DDBERR_NONE)
                throw new DDBException(GetLastError());

            var json = Marshal.PtrToStringAnsi(output);

            if (string.IsNullOrWhiteSpace(json))
                throw new DDBException("Unable to add");

            return JsonConvert.DeserializeObject<List<Entry>>(json);
        }

        [DllImport("ddb", EntryPoint = "DDBRemove")]
        static extern DDBError _Remove([MarshalAs(UnmanagedType.LPStr)] string ddbPath,
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                  int numPaths);

        public static void Remove(string ddbPath, string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                throw new DDBException("Path should not be null or empty");

            Remove(ddbPath, new[] { path });
        }
        public static void Remove(string ddbPath, string[] paths)
        {
            if (string.IsNullOrWhiteSpace(ddbPath))
                throw new DDBException("DDB path should not be null or empty");

            if (paths == null || paths.Any(string.IsNullOrWhiteSpace))
                throw new DDBException("One of the provided paths is null or empty");

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
            if (string.IsNullOrWhiteSpace(path))
                throw new DDBException("Path should not be null or empty");
            
            return Info(new[] { path }, recursive, maxRecursionDepth, withHash);
        }

        public static List<Entry> Info(string[] paths, bool recursive = false, int maxRecursionDepth = 0, bool withHash = false)
        {

            if (paths == null || paths.Any(string.IsNullOrWhiteSpace))
                throw new DDBException("One of the provided paths is null or empty");
            
            if (_Info(paths, paths.Length, out var output, "json", recursive, maxRecursionDepth, "auto", withHash) !=
                DDBError.DDBERR_NONE) throw new DDBException(GetLastError());

            var json = Marshal.PtrToStringAnsi(output);

            if (string.IsNullOrWhiteSpace(json))
                throw new DDBException("Unable get info");

            return JsonConvert.DeserializeObject<List<Entry>>(json);

        }

        [DllImport("ddb", EntryPoint = "DDBList")]
        static extern DDBError _List([MarshalAs(UnmanagedType.LPStr)] string ddbPath,
                                    [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                    int numPaths,
                                    out IntPtr output,
                                    [MarshalAs(UnmanagedType.LPStr)] string format,
                                    bool recursive,
                                    int maxRecursionDepth = 0);

        public static List<Entry> List(string ddbPath, string path, bool recursive = false, int maxRecursionDepth = 0)
        {
            if (string.IsNullOrWhiteSpace(path))
                throw new DDBException("Path should not be null or empty");

            return List(ddbPath, new[] { path }, recursive, maxRecursionDepth);
        }

        public static List<Entry> List(string ddbPath, string[] paths, bool recursive = false, int maxRecursionDepth = 0)
        {
            if (string.IsNullOrWhiteSpace(ddbPath))
                throw new DDBException("DDB path should not be null or empty");

            if (paths == null || paths.Any(string.IsNullOrWhiteSpace))
                throw new DDBException("One of the provided paths is null or empty");

            if (_List(ddbPath, paths, paths.Length, out var output, "json", recursive, maxRecursionDepth) !=
                DDBError.DDBERR_NONE) throw new DDBException(GetLastError());

            var json = Marshal.PtrToStringAnsi(output);

            if (string.IsNullOrWhiteSpace(json))
                throw new DDBException("Unable get list");

            return JsonConvert.DeserializeObject<List<Entry>>(json);

        }
    }
}
