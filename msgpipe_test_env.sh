#!/bin/bash
BGP_NET_DEBUG=1
DROP2USER=
[[ ! -z $DRUSER ]] && DROP2USER="-u $DRUSER"

function error_exit
{
    echo "$*" 1>&2
    popd &> /dev/null
    exit 1
}

function errcheck
{
    $* || error_exit $(echo "$* failed. aborting")
}

function dbg_msg() {
    [[ "$BGP_NET_DEBUG" == "1" ]] && echo "debug: $*"
}

function dbg_run() {
    [[ "$BGP_NET_DEBUG" == "1" ]] && $* || $* &> /dev/null
    [[ $? -eq 0 ]] || error_exit "failed running: $*"
}

GOBGP=$(which gobgp || echo $ENV_GOBGP_BIN)
GOBGPD=$(which gobgpd || echo $ENV_GOBGPD_BIN)
WORKDIR=./.gobgp_test_env

B1=b1
B1E0=b1-eth0
B1E0_IP=192.168.5.1
B1E0_IP_MASK=192.168.5.1/30
B1EXT=b1-ext
B1EXT_IP=192.168.10.1
B1EXT_IP_MASK=192.168.10.1/30
B1CONTROL=b1-control
B1CONTROL_IP=192.168.10.2
B1CONTROL_IP_MASK=192.168.10.2/30
B1BGPID=$WORKDIR/${B1}.pid
B1BGPAS=10
B1BGPIP=$B1E0_IP
B1BGPLOG=$WORKDIR/${B1}_bgp.log
B1BGPCONTIP=$B1EXT_IP

B2=b2
B2E0=b2-eth0
B2E0_IP=192.168.6.1
B2E0_IP_MASK=192.168.6.1/30
B2EXT=b2-ext
B2EXT_IP=192.168.20.1
B2EXT_IP_MASK=192.168.20.1/30
B2CONTROL=b2-control
B2CONTROL_IP=192.168.20.2
B2CONTROL_IP_MASK=192.168.20.2/30
B2BGPID=$WORKDIR/${B2}.pid
B2BGPAS=20
B2BGPIP=$B2E0_IP
B2BGPLOG=$WORKDIR/${B2}_bgp.log
B2BGPCONTIP=$B2EXT_IP

B3=b3
B3E0=b3-eth0
B3E0_IP=192.168.5.2
B3E0_IP_MASK=192.168.5.2/30
B3E1=b3-eth1
B3E1_IP=192.168.6.2
B3E1_IP_MASK=192.168.6.2/30
B3EXT=b3-ext
B3EXT_IP=192.168.30.1
B3EXT_IP_MASK=192.168.30.1/30
B3CONTROL=b3-control
B3CONTROL_IP=192.168.30.2
B3CONTROL_IP_MASK=192.168.30.2/30
B3BGPID=$WORKDIR/${B3}.pid
B3BGPAS=30
B3BGPIP=$B3EXT_IP
B3BGPLOG=$WORKDIR/${B3}_bgp.log
B3BGPCONTIP=$B3EXT_IP


B4=b4
B4E0=b4-eth0
B4EXT=b4-ext
B4CONTROL=b4-control

[[ "$(whoami)" != "root" ]] && error_exit "$0 must be run as root"
[[ ! -f $GOBGP ]] && error_exit "gobgp binary not defined, aborting ... "
[[ ! -f $GOBGPD ]] && error_exit "gobgp binary not defined, aborting ... "
[[ ! -d $WORKDIR ]] && mkdir $WORKDIR

function build_net_bgp()
{
    # create ns
    errcheck ip netns add $B1
    errcheck ip netns add $B2
    errcheck ip netns add $B3

    # create lo device
    errcheck ip netns exec $B1 ip link set lo up
    errcheck ip netns exec $B2 ip link set lo up
    errcheck ip netns exec $B3 ip link set lo up

    # create link devices to host machine
    errcheck ip netns exec $B1 ip link add $B1EXT type veth peer name $B1CONTROL netns 1
    errcheck ip netns exec $B2 ip link add $B2EXT type veth peer name $B2CONTROL netns 1
    errcheck ip netns exec $B3 ip link add $B3EXT type veth peer name $B3CONTROL netns 1

    # fix external ip for b1
    errcheck ip netns exec $B1 ip addr add dev $B1EXT $B1EXT_IP_MASK
    errcheck ip netns exec $B1 ip link set $B1EXT up

    errcheck ip addr add dev $B1CONTROL $B1CONTROL_IP_MASK
    errcheck ip link set $B1CONTROL up

    # fix external ip for b2
    errcheck ip netns exec $B2 ip addr add dev $B2EXT $B2EXT_IP_MASK
    errcheck ip netns exec $B2 ip link set $B2EXT up

    errcheck ip addr add dev $B2CONTROL $B2CONTROL_IP_MASK
    errcheck ip link set $B2CONTROL up

    # fix external ip for b3
    errcheck ip netns exec $B3 ip addr add dev $B3EXT $B3EXT_IP_MASK
    errcheck ip netns exec $B3 ip link set $B3EXT up

    errcheck ip addr add dev $B3CONTROL $B3CONTROL_IP_MASK
    errcheck ip link set $B3CONTROL up


    # connect nses
    errcheck ip netns exec $B1 ip link add $B1E0 type veth peer name $B3E0 netns $B3
    errcheck ip netns exec $B2 ip link add $B2E0 type veth peer name $B3E1 netns $B3

    # set up ip between nses
    errcheck ip netns exec $B1 ip addr add dev $B1E0 $B1E0_IP_MASK
    errcheck ip netns exec $B2 ip addr add dev $B2E0 $B2E0_IP_MASK
    errcheck ip netns exec $B3 ip addr add dev $B3E0 $B3E0_IP_MASK
    errcheck ip netns exec $B3 ip addr add dev $B3E1 $B3E1_IP_MASK
    errcheck ip netns exec $B1 ip link set $B1E0 up
    errcheck ip netns exec $B2 ip link set $B2E0 up
    errcheck ip netns exec $B3 ip link set $B3E0 up
    errcheck ip netns exec $B3 ip link set $B3E1 up
}

function bgp_prep_data()
{
    $b1gobgp global rib add 1.2.3.4/24
    $b1gobgp global rib add 2.2.3.0/16 nexthop 2.2.2.2

    $b2gobgp global rib add 10.2.3.0/24 nexthop 1.2.3.3
}

function bgp_set_neighbor()
{
    $b1gobgp neighbor add $B3E0_IP as $B3BGPAS
    $b2gobgp neighbor add $B3E1_IP as $B3BGPAS

    sleep 1
    # $b1gobgp neighbor #$B2BGPIP
    # $b2gobgp neighbor #$B1BGPIP

    #sleep 4
    #$b1gobgp neighbor $B2BGPIP enable
    #$b2gobgp neighbor $B1BGPIP enable

    #$b1gobgp neighbor #$B2BGPIP
    #$b2gobgp neighbor #$B1BGPIP

    while true
    do
        $b2gobgp neighbor $B3E0_IP enable
        $b2gobgp neighbor | grep never > /dev/null
        b2_ready=$?
        $b1gobgp neighbor $B3E1_IP enable
        $b1gobgp neighbor | grep never > /dev/null
        b1_ready=$?
        if [ $b1_ready -eq 1  -a  $b2_ready -eq 1 ]; then
            break
        fi
        sleep 2
        echo "not ready yet, trying again ... please wait"
    done

    # $b1gobgp neighbor #$B2BGPIP
    # $b2gobgp neighbor #$B1BGPIP
        
}

function clean_net_bgp()
{
    [[ -f $B1BGPID ]] && kill -9 $(cat $B1BGPID) && rm -f $B1BGPID
    [[ -f $B2BGPID ]] && kill -9 $(cat $B2BGPID) && rm -f $B2BGPID


    if [[ $(ip netns list) != "" ]]
    then
        ip link del $B1CONTROL
        ip link del $B2CONTROL
        ip link del $B3CONTROL
        if [[ "$1" == "force" ]]
        then
            echo "forcing cleanup"
            ip netns delete $B1
            ip netns delete $B2
            ip netns delete $B3
        else
            errcheck ip netns delete $B1
            errcheck ip netns delete $B2
            errcheck ip netns delete $B3
        fi
    fi
}

[[ $(id -u) != 0 ]] && error_exit "$0 need to be run as root, exiting ... "

export b1gobgp="$GOBGP -u $B1BGPCONTIP"
export b2gobgp="$GOBGP -u $B2BGPCONTIP"

if [[ "$1" == "clean" ]]
then
    # stoppoing bird gracefully
    ip netns exec $B3 birdc -s bird.ctl down
    # stopping nats gracefully
    ip netns exec $B3 nats-streaming-server --signal stop=b3_nats.pid

    clean_net_bgp


    rm -rf b3_nats.pid b3_nats.log
    rm -rf bird.msgpipe.log

    # if [[ $# > 1 ]]
    # then
    #     shift
    # fi
elif [[ "$1" == "force_clean" ]]
then

    clean_net_bgp force

elif [[ "$1" == "build" ]]
then
    if [[ $(ip netns list) != "" ]]
    then
        echo "environment is ALREADY running"
        exit 0
    fi

    build_net_bgp

    echo "$GOBGPD > $B1BGPLOG 2>&1 & 
    echo \$! > $B1BGPID" > .zb1
    chmod +x .zb1
    errcheck ip netns exec $B1 /bin/bash -c .zb1
    dbg_msg "gobgpd running on $B1: $B1BGPID ($(cat $B1BGPID))"
    rm .zb1

    echo "$GOBGPD > $B2BGPLOG 2>&1 & 
    echo \$! > $B2BGPID" > .zb2
    chmod +x .zb2
    errcheck ip netns exec $B2 /bin/bash -c .zb2
    dbg_msg "gobgpd running on $B2: $B2BGPID ($(cat $B2BGPID))"
    rm .zb2
    
    sleep 1 
    errcheck $GOBGP -u $B1BGPCONTIP global as $B1BGPAS router-id $B1BGPIP
    errcheck $GOBGP -u $B2BGPCONTIP global as $B2BGPAS router-id $B2BGPIP

    # export b1gobgp="$GOBGP -u $B1BGPCONTIP"
    # export b2gobgp="$GOBGP -u $B2BGPCONTIP"

    # start nats-streaming-server on the same machine as bird
    errcheck ip netns exec $B3 nats-streaming-server -mm 0  -mb 10000000000 -m 8222  -hbt 10s --pid b3_nats.pid -l b3_nats.log &
    # stoping the nats-streaming-server
    #  ip netns exec b3 nats-streaming-server --signal stop=b3_nats.pid

    # start bird with msgpipe support 
    # change argument to paramteres defined ealier ...
    errcheck ip netns exec $B3 bird -s bird.ctl -c bird.msgpipe.demo.conf  -P b3_bird.pid $DROP2USER

    bgp_prep_data
    # push some dumps 
    errcheck $GOBGP -u $B1BGPCONTIP mrt inject global mrt.dump
    errcheck $GOBGP -u $B2BGPCONTIP mrt inject global mrt.dump

    # enable msgpipe protocol
    birdc -s bird.ctl en piper

    bgp_set_neighbor

    
    echo ""
    echo "##### copy this #####"
    echo ""
    echo "alias b1gobgp=\"$GOBGP -u $B1BGPCONTIP\""
    echo "alias b2gobgp=\"$GOBGP -u $B2BGPCONTIP\""
    echo "alias b3birc=\"ip netns exec $B3 birdc -s bird.ctl"
    echo ""
    echo "##### and paste to terminal to enable control over gobgp in namespace #####"
    echo ""
    echo "#####################"
    echo "use b1gobgp and b2gobgp to access bgp instances"
    echo "for thorough gobgp commands see : https://github.com/osrg/gobgp/blob/master/docs/sources/cli-command-syntax.md"
    echo "#####################"
    echo ""
    echo "#####################"
    echo "use b3birdc to access bird"
    echo "..."
    echo "#####################"
    echo ""
    echo ""
    echo "#####################"
    echo "stopping gracefully nats-streaming server:"
    echo " ip netns exec b3 nats-streaming-server --signal stop=b3_nats.pid"
    echo "#####################"
    echo ""
    echo ""
    echo "#####################"
    echo "injecting mrt dump :"
    echo " b1gobgp mrt inject global 6762.mrt.65k.dump "
    echo " b2gobgp mrt inject global 2914.mrt.65k.dump "
    echo "#####################"
    echo ""


elif [[ "$1" == "status" ]]
then
    if [[ $(ip netns list) == "" ]]
    then
        echo "environment is NOT ready"
    else
        echo "environment is ready"
        ip netns list
        $b1gobgp global
        $b2gobgp global
    fi
else 
    echo "usage:"
    echo ""
    echo "  sudo $0 [ build | status | clean | force_clean ]"
    echo ""
    echo "parameters:"
    echo ""
    echo "  clean - clean the environment"
    echo "  force_clean - clean the environment forcefully"
    echo "  build - build the environment"
    echo "  status - show status of the environment"
    echo ""
    echo "NOTE: $0 create devices and uses ip address $B1CONTROL_IP_MASK and $B2CONTROL_IP_MASK for interacting with gobgp"
    echo ""
fi

