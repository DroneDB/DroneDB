using NUnit.Framework;
using System;
using System.IO;
using DDB.Bindings;
using System.Text.Json;
using System.Diagnostics;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using FluentAssertions;
using Microsoft.VisualStudio.TestPlatform.ObjectModel.Adapter;
using Newtonsoft.Json;

namespace DDB.Tests
{
    public class DroneDBTests
    {
        private const string BaseTestFolder = nameof(DroneDBTests);
        private const string TestFileUrl =
            "https://github.com/DroneDB/test_data/raw/master/test-datasets/drone_dataset_brighton_beach/DJI_0023.JPG";
        private const string Test1ArchiveUrl = "https://github.com/DroneDB/test_data/raw/master/registry/DdbFactoryTest/testdb1.zip";

        [SetUp]
        public void Setup()
        {
            DroneDB.RegisterProcess();
        }

        [Test]
        public void GetVersion_HasValue()
        {
            Assert.IsTrue(DroneDB.GetVersion().Length > 0, "Can call GetVersion()");
        }

        [Test]
        public void Init_NonExistant_Exception()
        {
            Action act = () => DroneDB.Init("nonexistant");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.Init(null);
            act.Should().Throw<DDBException>();

        }

        [Test]
        public void Init_EmptyFolder_Ok()
        {

            const string folder = "testInit";

            if (Directory.Exists(folder)) Directory.Delete(folder, true);
            Directory.CreateDirectory(folder);

            DroneDB.Init(folder).Should().Contain(folder);
            Directory.Exists(Path.Join(folder, ".ddb")).Should().BeTrue();
        }

        [Test]
        public void Add_NonExistant_Exception()
        {
            Action act = () => DroneDB.Add("nonexistant", "");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.Add("nonexistant", "test");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.Add(null, "test");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.Add("nonexistant", (string)null);
            act.Should().Throw<DDBException>();

        }

        [Test]
        public void EndToEnd_Add_Remove()
        {

            const string testFolder = "testAdd";

            if (Directory.Exists(testFolder)) Directory.Delete(testFolder, true);

            Directory.CreateDirectory(testFolder);
            DroneDB.Init(testFolder);

            File.WriteAllText(Path.Join(testFolder, "file.txt"), "test");
            File.WriteAllText(Path.Join(testFolder, "file2.txt"), "test");
            File.WriteAllText(Path.Join(testFolder, "file3.txt"), "test");

            Assert.Throws<DDBException>(() => DroneDB.Add(testFolder, "invalid"));

            var entry = DroneDB.Add(testFolder, Path.Join(testFolder, "file.txt"))[0];
            entry.Path.Should().Be("file.txt");
            entry.Hash.Should().NotBeNullOrWhiteSpace();

            var entries = DroneDB.Add(testFolder, new[] { Path.Join(testFolder, "file2.txt"), Path.Join(testFolder, "file3.txt") });
            entries.Should().HaveCount(2);

            DroneDB.Remove(testFolder, Path.Combine(testFolder, "file.txt"));

            Assert.Throws<DDBException>(() => DroneDB.Remove(testFolder, "invalid"));
        }

        [Test]
        public void Info_InvalidFile_Exception()
        {
            Action act = () => DroneDB.Info("invalid");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.Info((string)null);
            act.Should().Throw<DDBException>();
        }

        [Test]
        public void Info_GenericFile_Details()
        {

            const string testFolder = "testInfo";

            if (Directory.Exists(testFolder)) Directory.Delete(testFolder, true);
            Directory.CreateDirectory(testFolder);

            File.WriteAllText(Path.Join(testFolder, "file.txt"), "test");
            File.WriteAllText(Path.Join(testFolder, "file2.txt"), "test");

            var e = DroneDB.Info(Path.Join(testFolder, "file.txt"), withHash: true)[0];
            Assert.IsNotEmpty(e.Hash);

            // TODO: troubleshoot this and use 
            var es = DroneDB.Info(testFolder, true);
            Assert.AreEqual(2, es.Count);
            Assert.AreEqual(EntryType.Generic, es[0].Type);
            Assert.IsTrue(es[0].Size > 0);
            Assert.AreEqual(DateTime.Now.Year, es[0].ModifiedTime.Year);
        }

        [Test]
        public void Info_ImageFile_Details()
        {

            var expectedMeta = JsonConvert.DeserializeObject<Dictionary<string, string>>(
                @"{""cameraPitch"":""-89.9000015258789"",""cameraRoll"":""0.0"",""cameraYaw"":""43.79999923706055"",""captureTime"":""1466699554000.0"",""focalLength"":""3.4222222222222225"",""focalLength35"":""20.0"",""height"":""2250"",""make"":""DJI"",""model"":""FC300S"",""orientation"":""1"",""sensor"":""dji fc300s"",""sensorHeight"":""3.4650000000000003"",""sensorWidth"":""6.16"",""width"":""4000""}");

            using var tempFile = new TempFile(TestFileUrl, BaseTestFolder);

            var res = DroneDB.Info(tempFile.FilePath, withHash: true);

            res.Should().NotBeNull();
            res.Should().HaveCount(1);

            var info = res.First();

            info.Meta.Should().BeEquivalentTo(expectedMeta);
            info.Hash.Should().Be("246fed68dec31b17dc6d885cee10a2c08f2f1c68901a8efa132c60bdb770e5ff");
            info.Type.Should().Be(EntryType.GeoImage);
            info.Size.Should().Be(3876862);
            info.Depth.Should().Be(0);

        }

        [Test]
        public void List_Nonexistant_Exception()
        {
            Action act = () => DroneDB.List("invalid", "");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.List("invalid", "wefrfwef");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.List(null, "wefrfwef");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.List("invalid", (string)null);
            act.Should().Throw<DDBException>();

        }

        [Test]
        public void List_ExistingFile_Ok()
        {
            using var test = new TestFS(Test1ArchiveUrl, BaseTestFolder);

            var ddbPath = Path.Combine(test.TestFolder, "public", "default");

            var res = DroneDB.List(ddbPath, Path.Combine(ddbPath, "DJI_0027.JPG"));

            res.Should().HaveCount(1);
            var entry = res.First();

            entry.Should().BeEquivalentTo(JsonConvert.DeserializeObject<Entry>("{\"Path\":\"DJI_0027.JPG\",\"Hash\":\"3157958dd4f2562c8681867dfd6ee5bf70b6e9595b3e3b4b76bbda28342569ed\",\"Type\":3,\"Meta\":{\"cameraPitch\":\"-89.9000015258789\",\"cameraRoll\":\"0.0\",\"cameraYaw\":\"-131.3000030517578\",\"captureTime\":\"1466699584000.0\",\"focalLength\":\"3.4222222222222225\",\"focalLength35\":\"20.0\",\"height\":\"2250\",\"make\":\"DJI\",\"model\":\"FC300S\",\"orientation\":\"1\",\"sensor\":\"dji fc300s\",\"sensorHeight\":\"3.4650000000000003\",\"sensorWidth\":\"6.16\",\"width\":\"4000\"},\"mtime\":1491156087,\"Size\":3185449,\"Depth\":0,\"PointGeometry\":null,\"PolygonGeometry\":null}"));

        }

        [Test]
        public void List_AllFiles_Ok()
        {
            using var test = new TestFS(Test1ArchiveUrl, BaseTestFolder);

            var ddbPath = Path.Combine(test.TestFolder, "public", "default");

            var res = DroneDB.List(ddbPath, Path.Combine(ddbPath, "."), true);

            res.Should().HaveCount(26);

            res = DroneDB.List(ddbPath, ddbPath, true);

            res.Should().HaveCount(26);

        }


        [Test]
        public void Remove_Nonexistant_Exception()
        {
            Action act = () => DroneDB.Remove("invalid", "");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.Remove("invalid", "wefrfwef");
            act.Should().Throw<DDBException>();

            act = () => DroneDB.Remove(null, "wefrfwef");
            act.Should().Throw<DDBException>();

        }


        [Test]
        public void Remove_ExistingFile_Ok()
        {
            using var test = new TestFS(Test1ArchiveUrl, BaseTestFolder);

            const string fileName = "DJI_0027.JPG";

            var ddbPath = Path.Combine(test.TestFolder, "public", "default");

            var res = DroneDB.List(ddbPath, Path.Combine(ddbPath, fileName));
            res.Should().HaveCount(1);

            DroneDB.Remove(ddbPath, Path.Combine(ddbPath, fileName));

            res = DroneDB.List(ddbPath, fileName);
            res.Should().HaveCount(0);

        }

        [Test]
        public void Remove_AllFiles_Ok()
        {
            using var test = new TestFS(Test1ArchiveUrl, BaseTestFolder);

            const string fileName = ".";

            var ddbPath = Path.Combine(test.TestFolder, "public", "default");

            DroneDB.Remove(ddbPath, Path.Combine(ddbPath, fileName));

            var res = DroneDB.List(ddbPath, ".", true);
            res.Should().HaveCount(0);

        }

        [Test]
        public void Remove_NonexistantFile_Exception()
        {
            using var test = new TestFS(Test1ArchiveUrl, BaseTestFolder);

            const string fileName = "elaiuyhrfboeawuyirgfb";

            var ddbPath = Path.Combine(test.TestFolder, "public", "default");

            Action act = () => DroneDB.Remove(ddbPath, Path.Combine(ddbPath, fileName));

            act.Should().Throw<DDBException>();
        }

        [Test]
        public void Entry_Deserialization_Ok()
        {
            string json = "{'hash': 'abc', 'mtime': 5}";
            Entry e = JsonConvert.DeserializeObject<Entry>(json);
            Assert.IsTrue(e.ModifiedTime.Year == 1970);
        }

        [Test]
        [Explicit("Clean test directory")]
        public void Clean_Domain()
        {
            TempFile.CleanDomain(BaseTestFolder);
        }
    }
}