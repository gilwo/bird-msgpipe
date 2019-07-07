# howto run the demo of msgpipe

**NOTE - running the demo require root priviliges**

the demo create 3 namespaces 2 with gobgp bgp speakers and 1 with bird msgpipe.

the 2 gobgp instances are configured to talk to the bird msgpipe, the script will push 2 mrt dumps (one for each) in order to populate bird with add path routes information.

the instance where bird msgpipe runs also run nats-streaming-server

the demo client (see below instructions on how to build) is used to recieved and send route information.


## dependencies
the demo uses linux namespaces and require root permissions.

build according to the guidelines of [msgpipe-Howoto](msgpipe-Howto.MD),

demo require the following
* nats-streaming-server
* bird with message pipe support
* gobgp
* demo client

### building demo client
**require go > 1.12**
```bash
cd go
./build.sh
cd ..
```

## running the environment script
```bash
sudo DRUSER=$USER PATH=./:/path/to/gobgp/binaries:/path/to/nats-streaming-server/binary:$PATH ./msgpipe_test_env.sh build
```
## stoppoing the environment script
```bash
sudo DRUSER=$USER PATH=./:/path/to/gobgp/binaries:/path/to/nats-streaming-server/binary:$PATH ./msgpipe_test_env.sh clean
```

## demo program 
the demo program runs in 2 modes
automatic and manual

in automatic the route information are received and randomly select 1 per 1 to 5 routes to be published back to msgpipe
### example to run automatic mode
```bash
# populate the target table
./go/demo -H 192.168.30.1 -l debug

# clear the target table
./go/demo -H 192.168.30.1 -c -l debug
```

in manual mode the route information are passed as an argument
### example to run manual mode
```bash
# adding route
./go/demo -H 192.168.30.1 -m 1.1.1.1/32_2.2.2.2 -l debug
# removing route
./go/demo -H 192.168.30.1 -m 1.1.1.1/32_0.0.0.0 -l debug
```

**Disclaimer**

in the demo bird is configured to have one messagepipe protocol for sending and receiving messages. this is just and example and the possibilites are not limited to this kind of configuration 


## copy paste 

build the demo to tmp with all required packages and programs

```bash
mkdir /tmp/msgpipe-test
cd /tmp/message-test

git clone https://github.com/nats-io/nats-streaming-server.git nats-streaming-server.git
git clone https://github.com/osrg/gobgp gobgp.git
git clone https://github.com/ludocode/mpack.git mpack.git
git clone https://github.com/nats-io/nats.c.git nats.c.git
git clone https://github.com/gilwo/bird-msgpipe.git bird-msgpipe.git

cd mpack.git
git checkout v1.0
./tools/amalgamate.sh
pushd build/amalgamation/src/mpack
gcc -c mpack.c -o libmpack.a
popd
cd ..

cd nats.c.git
git checkout f0d2bb4 
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX=../install 
make && make install
cd ../..

cd nats-streaming-server.git
git checkout v0.15.1
go build nats-streaming-server.go
cd ..

cd gobgp.git
git checkout v2.5.0
go build -o gobgp cmd/gobgp/*.go
go build -o gobgpd cmd/gobgpd/{main,util}.go
cd ..

cd bird-msgpipe.git
git checkout msgpipe
autoreconf
./configure --with-cnatsdir=../nats.c.git/ --with-mpackdir=../mpack.git/build/amalgamation/src/mpack/
make

cd go
./build.sh
cd ..
```

from the same folder, start the environment

```bash
sudo DRUSER=$USER PATH=./:../gobgp.git/:../nats-streaming-server.git/:$PATH ./msgpipe_test_env.sh build
```

some commands and checks ups

```bash
# check target table is empty
./birdc -s bird.ctl sh route table dummy4 all

# run demo with manual route
./go/demo -H 192.168.30.1 -m 1.1.1.1/32_2.2.2.2 -l debug

# target table should be populated with the above route
./birdc -s bird.ctl sh route table dummy4 all

# remove the route
./go/demo -H 192.168.30.1 -m 1.1.1.1/32_0.0.0.0 -l debug

# target table should be empty again
./birdc -s bird.ctl sh route table dummy4 all

# populate with random selection from source table of piper protocol (valve4)
./go/demo -H 192.168.30.1 -l debug

# target table should be populated with routes
/birdc -s bird.ctl sh route table dummy4 all count

# remove all routes from target table (dummy4)
./go/demo -H 192.168.30.1 -c -l debug

# target table should be empty again
/birdc -s bird.ctl sh route table dummy4 all count
```

clean the environment
```bash
sudo DRUSER=$USER PATH=./:../gobgp.git/:../nats-streaming-server.git/:$PATH ./msgpipe_test_env.sh clean
```