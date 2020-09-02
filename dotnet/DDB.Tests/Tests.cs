using NUnit.Framework;
using System;
using System.IO;
using static DDB.Bindings.Exports;
using DDB.Bindings;
using System.Text.Json;
using System.Diagnostics;

namespace DDB.Tests
{
    public class Tests
    {
        [SetUp]
        public void Setup()
        {
            RegisterProcess();
        }

        [Test]
        public void testGetVersion()
        {
            Assert.IsTrue(GetVersion().Length > 0, "Can call GetVersion()");
        }

        [Test]
        public void testInit()
        {
            Assert.Throws<DDBException>(() => Init("nonexistant"));

            if (Directory.Exists("testInit")) Directory.Delete("testInit", true);
            Directory.CreateDirectory("testInit");
            Assert.IsTrue(Init("testInit").Contains("testInit"));
            Assert.IsTrue(Directory.Exists(Path.Join("testInit", ".ddb")));
        }

        [Test]
        public void testAddRemove()
        {
            Assert.Throws<DDBException>(() => Add("nonexistant", ""));

            if (Directory.Exists("testAdd")) Directory.Delete("testAdd", true);

            Directory.CreateDirectory("testAdd");
            Init("testAdd");

            File.WriteAllText(@"testAdd\file.txt", "test");
            File.WriteAllText(@"testAdd\file2.txt", "test");
            File.WriteAllText(@"testAdd\file3.txt", "test");

            Assert.Throws<DDBException>(() => Add("testAdd", "invalid"));

            Add("testAdd", @"testAdd\file.txt");
            Add("testAdd", new string[]{ @"testAdd\file2.txt", @"testAdd\file3.txt"});

            Remove("testAdd", @"testAdd\file.txt");
            Assert.Throws<DDBException>(() => Remove("testAdd", "invalid"));
        }

        [Test]
        public void testInfo()
        {
            Assert.Throws<DDBException>(() => Info("invalid"));

            if (Directory.Exists("testInfo")) Directory.Delete("testInfo", true);
            Directory.CreateDirectory("testInfo");

            File.WriteAllText(@"testInfo\file.txt", "test");


            string json = Info(@"testInfo\file.txt", withHash: true);
            Console.Write(json);
            //var utf8Reader = new Utf8JsonReader(json);

            //JsonSerializer.Deserialize(json);
        }
    }
}