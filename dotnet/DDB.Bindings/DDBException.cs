using System;
using System.Collections.Generic;
using System.Text;

namespace DDB.Bindings
{
    public class DDBException : Exception
    {
        public DDBException()
        {

        }
        
        public DDBException(string message) : base(message)
        {

        }

    }
}
