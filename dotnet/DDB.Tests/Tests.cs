using NUnit.Framework;
using System;
using System.IO;
using System.Reflection;
using static DDB.Lib.Exports;
using DDB.Lib;

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
    }
}