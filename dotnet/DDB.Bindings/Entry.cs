using Newtonsoft.Json;
using Newtonsoft.Json.Converters;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using Newtonsoft.Json.Linq;

namespace DDB.Bindings
{

    public class Entry
    {
        public string Path { get; set; }
        public string Hash { get; set; }
        public EntryType Type { get; set; }

        // TODO: this might change in the future
        public Dictionary<string, string> Meta { get; set; }

        [JsonProperty("mtime")]
        [JsonConverter(typeof(SecondEpochConverter))]
        public DateTime ModifiedTime { get; set; }

        public int Size { get; set; }
        public int Depth { get; set; }

        [JsonProperty("point_geom")]
        public JObject PointGeometry { get; set; }

        [JsonProperty("polygon_geom")]
        public JObject PolygonGeometry { get; set; }
    }

    // Unix seconds, with decimal places for millisecond precision
    class SecondEpochConverter : DateTimeConverterBase
    {
        private static readonly DateTime Epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);

        public override void WriteJson(JsonWriter writer, object value, JsonSerializer serializer)
        {
            writer.WriteRawValue((((DateTime)value - Epoch).TotalMilliseconds / 1000.0).ToString(CultureInfo.InvariantCulture));
        }

        public override object ReadJson(JsonReader reader, Type objectType, object existingValue, JsonSerializer serializer)
        {
            if (reader.Value == null) { return null; }

            double val;

            if (reader.ValueType == typeof(int)) val = (int)reader.Value;
            else if (reader.ValueType == typeof(long)) val = (long)reader.Value;
            else if (reader.ValueType == typeof(double)) val = (double)reader.Value;
            else throw new InvalidCastException();

            return Epoch.AddSeconds(val);
        }
    }
}
