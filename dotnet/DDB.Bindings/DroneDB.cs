using Newtonsoft.Json;
using System;
using System.Collections.Generic;
using System.IO;
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

            try
            {

                if (_Init(directory, out var outPath) == DDBError.DDBERR_NONE)
                    return Marshal.PtrToStringAnsi(outPath);

            }
            catch (Exception ex)
            {
                throw new DDBException($"Error in calling ddb lib. Last error: \"{GetLastError()}\", check inner exception for details", ex);
            }

            throw new DDBException(GetLastError());


        }

        [DllImport("ddb", EntryPoint = "DDBAdd")]
        static extern DDBError _Add([MarshalAs(UnmanagedType.LPStr)] string ddbPath,
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                  int numPaths, out IntPtr output, bool recursive);

        public static List<Entry> Add(string ddbPath, string path, bool recursive = false)
        {
            return Add(ddbPath, path != null ? new[] { path } : null, recursive);
        }

        public static List<Entry> Add(string ddbPath, string[] paths, bool recursive = false)
        {

            try
            {
                if (_Add(ddbPath, paths, paths?.Length ?? 0, out var output, recursive) != DDBError.DDBERR_NONE)
                    throw new DDBException(GetLastError());

                var json = Marshal.PtrToStringAnsi(output);

                if (string.IsNullOrWhiteSpace(json))
                    throw new DDBException("Unable to add");

                return JsonConvert.DeserializeObject<List<Entry>>(json);
            }
            catch (Exception ex)
            {
                throw new DDBException($"Error in calling ddb lib. Last error: \"{GetLastError()}\", check inner exception for details", ex);
            }

        }

        [DllImport("ddb", EntryPoint = "DDBRemove")]
        static extern DDBError _Remove([MarshalAs(UnmanagedType.LPStr)] string ddbPath,
                                  [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] paths,
                                  int numPaths);

        public static void Remove(string ddbPath, string path)
        {
            Remove(ddbPath, path != null ? new[] { path } : null);
        }
        public static void Remove(string ddbPath, string[] paths)
        {
            try
            {
                if (_Remove(ddbPath, paths, paths?.Length ?? 0) != DDBError.DDBERR_NONE)
                    throw new DDBException(GetLastError());
            }
            catch (Exception ex)
            {
                throw new DDBException($"Error in calling ddb lib. Last error: \"{GetLastError()}\", check inner exception for details", ex);
            }
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
            return Info(path != null ? new[] { path } : null, recursive, maxRecursionDepth, withHash);
        }

        public static List<Entry> Info(string[] paths, bool recursive = false, int maxRecursionDepth = 0, bool withHash = false)
        {

            try
            {
                if (_Info(paths, paths?.Length ?? 0, out var output, "json", recursive, maxRecursionDepth, "auto", withHash) !=
                    DDBError.DDBERR_NONE) throw new DDBException(GetLastError());

                var json = Marshal.PtrToStringAnsi(output);

                if (string.IsNullOrWhiteSpace(json))
                    throw new DDBException("Unable get info");

                return JsonConvert.DeserializeObject<List<Entry>>(json);

            }
            catch (Exception ex)
            {
                throw new DDBException($"Error in calling ddb lib. Last error: \"{GetLastError()}\", check inner exception for details", ex);
            }
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
            return List(ddbPath, path != null ? new[] { path } : null, recursive, maxRecursionDepth);
        }

        public static List<Entry> List(string ddbPath, string[] paths, bool recursive = false, int maxRecursionDepth = 0)
        {
            try
            {

                if (_List(ddbPath, paths, paths?.Length ?? 0, out var output, "json", recursive, maxRecursionDepth) !=
                    DDBError.DDBERR_NONE) throw new DDBException(GetLastError());

                var json = Marshal.PtrToStringAnsi(output);

                if (string.IsNullOrWhiteSpace(json))
                    throw new DDBException("Unable get list");

                return JsonConvert.DeserializeObject<List<Entry>>(json);

            }
            catch (Exception ex)
            {
                throw new DDBException($"Error in calling ddb lib. Last error: \"{GetLastError()}\", check inner exception for details", ex);
            }

        }
    }
}
