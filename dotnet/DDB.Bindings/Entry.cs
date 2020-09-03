using Newtonsoft.Json;
using Newtonsoft.Json.Converters;
using System;
using System.Collections.Generic;
using System.Text;

namespace DDB.Bindings
{

    public class Entry
    {
        public string Path { get; set; }
        public string Hash { get; set; }
        public EntryType Type { get; set; }

        // TODO: this might change in the future
        public Dictionary<string,string> Meta { get; set; }

        [JsonProperty("mtime")]
        [JsonConverter(typeof(SecondEpochConverter))]
        public DateTime ModifiedTime { get; set; }

        public int Size { get; set; }
        public int Depth { get; set; }

        public string PointGeometry { get; set; }

        public string PolygonGeometry { get; set; }
    }

    // Unix seconds, with decimal places for millisecond precision
    class SecondEpochConverter : DateTimeConverterBase
    {
        private static readonly DateTime _epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);

        public override void WriteJson(JsonWriter writer, object value, JsonSerializer serializer)
        {
            writer.WriteRawValue((((DateTime)value - _epoch).TotalMilliseconds / 1000.0).ToString());
        }

        public override object ReadJson(JsonReader reader, Type objectType, object existingValue, JsonSerializer serializer)
        {
            if (reader.Value == null) { return null; }

            double val = 0;
            if (reader.ValueType == typeof(int)) val = (int)reader.Value;
            else if (reader.ValueType == typeof(Int64)) val = (Int64)reader.Value;
            else if (reader.ValueType == typeof(double)) val = (double)reader.Value;
            else throw new InvalidCastException();

            return _epoch.AddSeconds(val);
        }
    }
}
