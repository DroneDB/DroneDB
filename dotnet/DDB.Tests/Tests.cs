using NUnit.Framework;
using System;
using System.IO;
using DDB.Bindings;
using System.Text.Json;
using System.Diagnostics;
using System.Collections.Generic;
using Newtonsoft.Json;

namespace DDB.Tests
{
    public class Tests
    {
        [SetUp]
        public void Setup()
        {
            DroneDB.RegisterProcess();
        }

        [Test]
        public void testGetVersion()
        {
            Assert.IsTrue(DroneDB.GetVersion().Length > 0, "Can call GetVersion()");
        }

        [Test]
        public void testInit()
        {
            Assert.Throws<DDBException>(() => DroneDB.Init("nonexistant"));

            if (Directory.Exists("testInit")) Directory.Delete("testInit", true);
            Directory.CreateDirectory("testInit");
            Assert.IsTrue(DroneDB.Init("testInit").Contains("testInit"));
            Assert.IsTrue(Directory.Exists(Path.Join("testInit", ".ddb")));
        }

        [Test]
        public void testAddRemove()
        {
            Assert.Throws<DDBException>(() => DroneDB.Add("nonexistant", ""));
            Assert.Throws<DDBException>(() => DroneDB.Add("nonexistant", "test"));

            if (Directory.Exists("testAdd")) Directory.Delete("testAdd", true);

            Directory.CreateDirectory("testAdd");
            DroneDB.Init("testAdd");

            File.WriteAllText(@"testAdd\file.txt", "test");
            File.WriteAllText(@"testAdd\file2.txt", "test");
            File.WriteAllText(@"testAdd\file3.txt", "test");

            Assert.Throws<DDBException>(() => DroneDB.Add("testAdd", "invalid"));

            DroneDB.Add("testAdd", @"testAdd\file.txt");
            DroneDB.Add("testAdd", new string[]{ @"testAdd\file2.txt", @"testAdd\file3.txt"});

            DroneDB.Remove("testAdd", @"testAdd\file.txt");
            Assert.Throws<DDBException>(() => DroneDB.Remove("testAdd", "invalid"));
        }

        [Test]
        public void testInfo()
        {
            Assert.Throws<DDBException>(() => DroneDB.Info("invalid"));

            if (Directory.Exists("testInfo")) Directory.Delete("testInfo", true);
            Directory.CreateDirectory("testInfo");

            File.WriteAllText(@"testInfo\file.txt", "test");
            File.WriteAllText(@"testInfo\file2.txt", "test");

            Entry e = DroneDB.Info(@"testInfo\file.txt", withHash: true);
            Assert.IsNotEmpty(e.Hash);

            List<Entry> es = DroneDB.Info(new string[] { @"testInfo"}, recursive: true);
            Assert.AreEqual(2, es.Count);
            Assert.AreEqual(EntryType.Generic, es[0].Type);
            Assert.IsTrue(es[0].Size > 0);
            Assert.AreEqual(DateTime.Now.Year, es[0].ModifiedTime.Year); 
        }

        [Test]
        public void testEntrySerialization()
        {
            string json = "{'hash': 'abc', 'mtime': 5}";
            Entry e = JsonConvert.DeserializeObject<Entry>(json);
            Assert.IsTrue(e.ModifiedTime.Year == 1970);
        }
    }
}