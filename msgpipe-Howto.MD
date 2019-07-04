# howto build and run bird with msgpipe support using nats streaming server

this version of bird with msgpipe support is highly expermintal version.
its in early testing stages and should be tested extensively.

## how to build
before building this version there are couple of prerequisits libraries 
* github.com/nats-io/nats.c.git
* github.com/ludocode/mpack.git

### first build cnats:

```bash
git clone https://github.com/nats-io/nats.c.git nats.c.git
cd nats.c.git
# no tags for this, but this is the commit id that bird-msgpipe was build with
git checkout f0d2bb4 
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX=../install 
make && make install
cd ..
cd ..
```

### build mpack amalgamation

```bash
git clone https://github.com/ludocode/mpack.git mpack.git
cd mpack.git
# no tags for this, but this is the commit id that bird-msgpipe was build with
git checkout 5b26983
./tools/amalgamate.sh
cd build/amalgamation/src/mpack
# this is a kinda hack at this stage
gcc -c mpack.c -o libmpack.a
cd -
cd ..
```

### build bird:
```bash
autoreconf
./configure --with-cnatsdir=/path/to/nats.c.git/ --with-mpackdir=/path/to/mpack/amalgamation/src/mpack/
make
```

## how to configure bird.conf

msgpipe protocol is very similiar to pipe protocol with slight difference.
the peer table is not populated automatically upon update of the main table.
in addition there are couple of additional field in the msgpipe protocol required for it to be operational

### sample configuration of msgpipe protocol
```bash
protocol msgpipe piper {
    description "send updates to subscriber pipe and recieve from publisher";
    # primary table - upon update will trigger publish of the message 
    table valve4;
    # secondary table - upon update from the subscribed channel on the nats streaming server this table will be polulated 
    peer table dummy4;

    export all;

    # this is the publish channel for updates
    pubdata "fooData";
    # this is the publish channel for control
    pubcontrol "fooControl";
    # this is the subscriiber channel 
    sub "bar";
}
```

## how to run 
in order to run bird-msgpipe it is required to run nats-streaming-server on the local machine where bird-msgpipe is running

### building nats-streaming-server from source 
nats-streaming-server version used in the development and should be tested against is v0.15.1
go version used for building nats-streaming-server is v1.12.1

```bash
git clone https://github.com/nats-io/nats-streaming-server.git nats-streaming-server.git
cd nats-streaming-server.git
git checkout v0.15.1
go build nats-streaming-server.go
```

### running nats-streaming-server 
``` ./nats-streaming-server -mm 0 -mb 10000000000 -m 8222 ```

this will run nats-streaming server with defaults values for all settings beside:
* no limit on number of messages
* limit of 10G bytes limitation of memory usage
* running http monitoring on port 8222 on local host


## using gobgp as an injector of mrt dumps

### build gobgp

**require go > 1.12**
```bash
git clone http://github.com/osrg/gobgp gobgp.git
cd gobgp
git checkout v2.5.0
go build -o gobgp cmd/gobgp/*.go
go build -o gobgpd cmd/gobgpd/{main,util}.go
cd ..
```


