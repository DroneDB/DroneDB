using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using ICSharpCode.SharpZipLib.GZip;
using ICSharpCode.SharpZipLib.Tar;
using ZipFile = System.IO.Compression.ZipFile;

namespace DDB.Tests
{
    public static class Utils
    {
        public static string RandomString(int length)
        {
            const string valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
            var res = new StringBuilder();
            using var rng = new RNGCryptoServiceProvider();
            var uintBuffer = new byte[sizeof(uint)];

            while (length-- > 0)
            {
                rng.GetBytes(uintBuffer);
                var num = BitConverter.ToUInt32(uintBuffer, 0);
                res.Append(valid[(int) (num % (uint) valid.Length)]);
            }

            return res.ToString();
        }

        private const int BytesToRead = sizeof(long);

        public static bool FilesAreEqual(string first, string second)
        {
            return FilesAreEqual(new FileInfo(first), new FileInfo(second));
        }

        public static bool FilesAreEqual(FileInfo first, FileInfo second)
        {
            if (first.Length != second.Length)
                return false;

            if (string.Equals(first.FullName, second.FullName, StringComparison.OrdinalIgnoreCase))
                return true;

            var iterations = (int) Math.Ceiling((double) first.Length / BytesToRead);

            using var fs1 = first.OpenRead();
            using var fs2 = second.OpenRead();

            var one = new byte[BytesToRead];
            var two = new byte[BytesToRead];

            for (var i = 0; i < iterations; i++)
            {
                fs1.Read(one, 0, BytesToRead);
                fs2.Read(two, 0, BytesToRead);

                if (BitConverter.ToInt64(one, 0) != BitConverter.ToInt64(two, 0))
                    return false;
            }

            return true;
        }

        public static TValue SafeGetValue<TKey, TValue>(this IDictionary<TKey, TValue> dictionary, TKey key)
        {
            TValue value;

            return !dictionary.TryGetValue(key, out value) ? default(TValue) : value;
        }

        public static TValueOut? SafeGetValue<TKey, TValue, TValueOut>(this IDictionary<TKey, TValue> dictionary,
            TKey key, Func<TValue, TValueOut> selector) where TValueOut : struct
        {
            TValue value;

            return !dictionary.TryGetValue(key, out value) ? null : (TValueOut?) selector(value);
        }

        /// <summary>
        /// Ensures that the sqlite database folder exists 
        /// </summary>
        /// <param name="connstr"></param>
        public static void EnsureFolderCreated(string connstr)
        {
            var segments = connstr.Split(';', StringSplitOptions.RemoveEmptyEntries);

            foreach (var segment in segments)
            {
                var fields = segment.Split('=');

                if (string.Equals(fields[0], "Data Source", StringComparison.OrdinalIgnoreCase))
                {
                    var dbPath = fields[1];

                    var folder = Path.GetDirectoryName(dbPath);

                    if (!Directory.Exists(folder))
                    {
                        Directory.CreateDirectory(folder);
                    }
                }
            }
        }

        public static void SmartExtractFolder(string archive, string dest, bool overwrite = true)
        {

            var ext = Path.GetExtension(archive).ToLowerInvariant();

            if (ext == ".tar.gz" || ext == ".tgz")
                ExtractTGZ(archive, dest);
            else
                ZipFile.ExtractToDirectory(archive, dest, overwrite);

        }

        public static void ExtractTGZ(string archive, string destFolder)
        {
            using Stream inStream = File.OpenRead(archive);
            using Stream gzipStream = new GZipInputStream(inStream);

            var tarArchive = TarArchive.CreateInputTarArchive(gzipStream, Encoding.UTF8);
            tarArchive.ExtractContents(destFolder);
            tarArchive.Close();

            gzipStream.Close();
            inStream.Close();
        }

        public static string ComputeSha256Hash(string str)
        {
            return ComputeSha256Hash(Encoding.UTF8.GetBytes(str));
        }

        public static string ComputeSha256Hash(byte[] rawData)
        {
            // Create a SHA256   
            using var sha256Hash = SHA256.Create();
            // ComputeHash - returns byte array  
            var bytes = sha256Hash.ComputeHash(rawData);

            // Convert byte array to a string   
            var builder = new StringBuilder();
            foreach (var t in bytes)
                builder.Append(t.ToString("x2"));

            return builder.ToString();
        }

        private const string SmartFileCacheFolder = "SmartFileCache";

        /// <summary>
        /// Downloads a file using a rudimentary cache in temp folder
        /// </summary>
        /// <param name="url"></param>
        /// <param name="path"></param>
        public static void SmartDownloadFile(string url, string path)
        {
            var uri = new Uri(url);
            var fileName = uri.Segments.Last();

            var smartFileCacheFolder = Path.Combine(Path.GetTempPath(), SmartFileCacheFolder);

            if (!Directory.Exists(smartFileCacheFolder))
                Directory.CreateDirectory(smartFileCacheFolder);

            var cachedFilePath = Path.Combine(smartFileCacheFolder, fileName);

            if (!File.Exists(cachedFilePath))
            {
                var client = new WebClient();
                client.DownloadFile(url, cachedFilePath);
            }

            File.Copy(cachedFilePath, path, true);

        }

        /// <summary>
        /// Downloads a file using a rudimentary cache in temp folder
        /// </summary>
        /// <param name="url"></param>
        public static byte[] SmartDownloadData(string url)
        {

            var tmp = Path.GetTempFileName();

            SmartDownloadFile(url, tmp);

            var data = File.ReadAllBytes(tmp);

            File.Delete(tmp);

            return data;

        }

    }


}
