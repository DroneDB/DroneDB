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
            Assert.AreEqual(entry.Path, "file.txt");
            Assert.IsTrue(entry.Hash.Length > 0);
            
            var entries = DroneDB.Add(testFolder, new string[]{ Path.Join(testFolder, "file2.txt"), Path.Join(testFolder, "file3.txt")});
            Assert.AreEqual(entries.Count, 2);

            DroneDB.Remove(testFolder, Path.Join(testFolder, "file.txt"));
            
            Assert.Throws<DDBException>(() => DroneDB.Remove(testFolder, "invalid"));
        }

        [Test]
        public void Info_InvalidFile_Exception()
        {
            Action act = () => DroneDB.Info("invalid");
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
            const string testFileUrl =
                "https://github.com/DroneDB/test_data/raw/master/test-datasets/drone_dataset_brighton_beach/DJI_0023.JPG";

            var expectedMeta = JsonConvert.DeserializeObject<Dictionary<string, string>>(
                @"{""cameraPitch"":""-89.9000015258789"",""cameraRoll"":""0.0"",""cameraYaw"":""43.79999923706055"",""captureTime"":""1466699554000.0"",""focalLength"":""3.4222222222222225"",""focalLength35"":""20.0"",""height"":""2250"",""make"":""DJI"",""model"":""FC300S"",""orientation"":""1"",""sensor"":""dji fc300s"",""sensorHeight"":""3.4650000000000003"",""sensorWidth"":""6.16"",""width"":""4000""}");

            using var tempFile = new TempFile(testFileUrl, nameof(DroneDBTests));

            var res = DroneDB.Info(tempFile.FilePath, withHash:true);

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

        }

        //[Test]
        //public void testEntrySerialization()
        //{
        //    string json = "{'hash': 'abc', 'mtime': 5}";
        //    Entry e = JsonConvert.DeserializeObject<Entry>(json);
        //    Assert.IsTrue(e.ModifiedTime.Year == 1970);
        //}
    }
}