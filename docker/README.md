# Dockerised DroneDB

## Build the container

This dockerfile builds DroneDB with its dependencies at June 12 2020.

To use it, clone the DroneDB repository and head to the `docker/ubuntu` directory.

Then run:

`docker build . -t dronedb/build`

(`-t` can be anything - use something you remember).

## Using DroneDB in docker

Navigate to the root of a directory tree holding your imagery - for example if your data structure looks like:

`/drone-images`
`/drone-images/mission1/imagery/`
`/drone-images/mission2/imagery/`
`...``

...navigate to `/drone-images`.

To get starteed, run DroneDB like:

`docker run -it -v $(pwd):/dronedb-data dronedb/build init`

...to initialise a database. `/dronedb-data` is mapped as a working directory inside the container. You should now see a `.ddb` directory in your `/drone-images` directory:

`drone-images/.ddb`

To add imagery to the database, run:

`docker run -it -v $(pwd):/dronedb-data dronedb/build add mission1/imagery/*.JPG`

...you should see the sqlite file inside `.ddb` grow in size, and some output (one line per image) on your terminal.

## Caveats

This docker build may have unexpected misbehaviour - feel free to fix wonky things via pull requests!
