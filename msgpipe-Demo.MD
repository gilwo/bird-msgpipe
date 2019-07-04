# howto run the demo of msgpipe

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
./build
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
./go/demo -H 192.168.30.1 -l debug
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
