# Database files

Please download the database files [here](https://cdn.bertold.org/zonedetect/db/db.zip) or create them using the builder.

To use the builder, first install [shapelib](https://github.com/OSGeo/shapelib), then:

    cd database/builder
    ./makedb.sh
    
This will create database files in `out` and `out_v1`, as well as a `db.zip` containing both directories.

The files in the folder out\_v1/ use a newer format and use less space to encode the same information.

The numbers in on the file names indicate the resolution. The `*21` file has a higher resolution for storing the borders, but it is larger. The `*16` file has a longitude resolution of 0.0055 degrees (~0.5km) and the `*21` file has 0.00017 degrees (~20m)
