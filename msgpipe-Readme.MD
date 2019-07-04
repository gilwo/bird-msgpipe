# bird msgpipe

bird msgpipe is an experimental way to control bgp routes information using an external program via [nats streaming messaging system](https://nats-io.github.io/docs/nats_streaming/intro.html).

msgpipe protocol rely on a nats streaming server to run on the local machine where bird-msgpipe is running.
(this is a current limitation which can easy be changed and pass as a paramter in the msgpipe protocol configuration structure)

msgpipe protocol pack the bgp route information for each network it recieve update for using the [msgpack](http://msgpack.org) protocol 

schema for packing is correlate with how route information is saved for bgp routes in bird with some additional fields

**TODO**: add detailed description on msgpack structure of message packed 

#### sent messages (on the published control channle of nats-streaming-server):
* meta message for start or stop of the data channel (corrleate to msgpiper protcol enable and disable respectively)

#### sent messages (on the published data channle of nats-streaming-server):
* update route information for a netowrk prefix 

#### recieved messages (on the subscribed channel of nats-streaming-server) is roughly of 2 types:
* known networks information 
    - this is an existing route information for an existing network in main table of msgpipe protocol 
    - the route information will be looked in the source table (using prefix and nexthop) and if exists will be updated the peer table, otherwise nothing will happen.
* manual crafted networks 
    - newly generated networks - this is currently no implmented fully as a bgp route information and limited to prefix address and nexthop address


## TODO stuff
* convert connecting to nats-streaming-server using an event loop : more info within nats.c [Using an Event Loop Library](https://github.com/nats-io/nats.c#using-an-event-loop-library)
* incoming manual updates from the subscribed message channel are currently converted to hybrid static route informatoin - this need to be changed in order to support proper bgp update message, or maybe something else  ?!? .
* add proper counters to number of routes sent and recieved on the message channels.
* improve losing a connection to the nats streaming server
* move additioanl configuration elements from code to msgpipe protocol configuration
  * URL of nats streaming server/cluster
* counters fix
* better parsing of the incoming message



## known issues
* sometimes the protocl status is down while it suppose to be up and when enabling it, there is an error of already up.
* there is no validation for incoming messages, which mean it can crash bird
* mpack of zero values ? 