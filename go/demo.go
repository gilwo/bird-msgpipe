package main

import (
	/*
		"bufio"
		"runtime"
		"sync"
	*/

	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math/rand"
	"net"
	"os"
	"runtime/pprof"
	"strconv"
	"strings"
	"time"

	"github.com/gilwo/ipnumbers"
	"github.com/nats-io/go-nats"
	"github.com/vmihailenco/msgpack"

	stan "github.com/nats-io/go-nats-streaming"
	//"github.com/gilwo/nradix"

	"github.com/jessevdk/go-flags"
	log "github.com/sirupsen/logrus"
)

const ()

var (
	stanClusterID = "test-cluster"
	stanClientID  = "client-go-yy-other"
	stanURL       = "nats://127.0.0.1:4222"

	subcData    stan.Subscription
	subcControl stan.Subscription

	subControlName = "fooControl"
	subDataName    = "fooData"
	pubDataName    = "bar"

	sc stan.Conn

	Opts struct {
		CPUProfile string `short:"C" description:"enable CPU profiling and save to file" hidden:"1"`
		MEMProfile string `short:"M" description:"enable Memory profiling and save to file" hidden:"1"`
		LogLevel   string `short:"l" long:"loglevel" description:"global log level" choice:"trace" choice:"debug" choice:"info" choice:"warning" choice:"error" choice:"fatal" choice:"panic"`
		StanHost   string `short:"H" description:"stan host" hidden:"0"`
		Manual     string `short:"m" description:"manual publishing or route" hidden:"0"`
		Clear      bool   `short:"c" description:"clear route table" hidden:"0"`
	}

	count = 0
)

/// message type struct --- start ---
type myMsg struct {
	Which     string     `json:"which"`
	Prefix    *net.IPNet `json:"-"`
	CurrNH    net.IP     `json:"nexthop"`
	BestNH    net.IP     `json:"best"`
	Oldptr    uint64     `json:"-"`
	Newptr    uint64     `json:"-"`
	timeStamp int64      //`json:"-"`
	msgSeq    uint64     //`json:"-"`
}

func NewMsg() *myMsg {
	return &myMsg{}
}

func (m *myMsg) UnmarshalJSON(data []byte) error {
	type Alias myMsg
	aux := &struct {
		*Alias
		PrefixString string `json:"prefix"`
		OldString    string `json:"old"`
		NewString    string `json:"new"`
	}{
		Alias: (*Alias)(m),
	}
	// log.Errorf("before internal unmarshal: %#v\n", data)
	if err := json.Unmarshal(data, &aux); err != nil {
		return err
	}
	clog(log.DebugLevel, "!! aux: %#v\n", func() interface{} { return aux })
	_, n, err := net.ParseCIDR(aux.PrefixString)
	if err != nil {
		return err
	}
	m.Prefix = n

	old, err := strconv.ParseUint(aux.OldString, 16, 64)
	if err != nil {
		return err
	}
	m.Oldptr = old
	new, err := strconv.ParseUint(aux.NewString, 16, 64)
	if err != nil {
		return err
	}
	m.Newptr = new
	return nil
}

func (m *myMsg) MarshalJSON() ([]byte, error) {
	type Alias myMsg
	return json.Marshal(&struct {
		*Alias
		PrefixString string `json:"prefix"`
		OldString    string `json:"old"`
		NewString    string `json:"new"`
	}{
		Alias:        (*Alias)(m),
		PrefixString: m.Prefix.String(),
		OldString:    fmt.Sprintf("%x", m.Oldptr),
		NewString:    fmt.Sprintf("%x", m.Newptr),
	})
}

func (m myMsg) String() string {
	return fmt.Sprintf("which: %s, prefix: %s, nh: %s, best: %s, old: %x, new: %x, timestamp: %v (%s)",
		m.Which, m.Prefix, m.CurrNH, m.BestNH, m.Oldptr, m.Newptr, m.timeStamp, time.Unix(0, m.timeStamp))
}

/// message type struct --- stop ---

func clog(lvl log.Level, fmtStr string, argFuncs ...func() interface{}) {
	if log.GetLevel() >= lvl {
		/*
			    ddata := []interface{}{}
				for _, f := range argFuncs {
					ddata = append(ddata, f())
				}
				//reverse order as the args pushed back
				dout := []interface{}{}
				for _, e := range ddata {
					dout = append(dout, e)
				}
		*/
		l := len(argFuncs)
		dout := make([]interface{}, l)
		for i, e := range argFuncs {
			dout[i] = e()
		}

		switch lvl {
		case log.PanicLevel:
			log.Panicf(fmtStr, dout...)
		case log.FatalLevel:
			log.Fatalf(fmtStr, dout...)
		case log.ErrorLevel:
			log.Errorf(fmtStr, dout...)
		case log.WarnLevel:
			log.Warnf(fmtStr, dout...)
		case log.InfoLevel:
			log.Infof(fmtStr, dout...)
		case log.DebugLevel:
			log.Debugf(fmtStr, dout...)
		case log.TraceLevel:
			log.Tracef(fmtStr, dout...)

		}
	}
}

func onMsgData(m *stan.Msg) {
	clog(log.TraceLevel, "received message on [%v] seq [%v]: %s, redelivared: [%v]\n",
		func() interface{} { return m.Subject },
		func() interface{} { return m.Sequence },
		func() interface{} { return string(m.Data) },
		func() interface{} { return m.Redelivered },
	)
	i2 := decodeMsg(m.Data)

	pubFunc := func() {
		dataBuf := &bytes.Buffer{}
		msgEncoder := msgpack.NewEncoder(dataBuf)
		msgEncoder.UseJSONTag(true)
		err := msgEncoder.Encode(i2)
		if err != nil {
			clog(log.ErrorLevel, "msgpack encoding failed: %s",
				func() interface{} { return err })
		}
		retStr, err := sc.PublishAsync(pubDataName, dataBuf.Bytes(), func(guid string, err error) {
			if err != nil {
				clog(log.ErrorLevel, "publish prefix - ack failed for message guid: %s, %s\n",
					func() interface{} { return guid },
					func() interface{} { return err })
			}
		})
		if err != nil {
			clog(log.ErrorLevel, "publish prefix - failed for message %s (guid: %s), err %s",
				func() interface{} { return dataBuf },
				func() interface{} { return retStr },
				func() interface{} { return err })
		}
		clog(log.TraceLevel, "published: \n%v\n%v\n",
			func() interface{} { return i2 },
			func() interface{} { return hex.Dump(dataBuf.Bytes()) })
	}

	if Opts.Clear {
		i2.Which = whichDel
		go pubFunc()
	} else {
		i2.Which = whichAdd
		count++
		if count%(rand.Intn(5)+1) == 0 {
			go pubFunc()
		}
	}

	m.Ack()
}

func handleMsgControl(m *myMsg) {
	var err error

	clog(log.InfoLevel, "got control message %v",
		func() interface{} { return m })
	switch m.Which {
	case "start":
		clog(log.InfoLevel, "received start message, subscribing to data channel")

		clog(log.InfoLevel, "subscribing to data channel from ts: %v(%v)",
			func() interface{} { return time.Unix(0, m.timeStamp) },
			func() interface{} { return m.timeStamp })
		subcData, err = sc.Subscribe(
			subDataName,
			onMsgData,
			// stan.StartAtTimeDelta(time.Millisecond*500), // TODO: maybe this should correlate to bgp gracefull time ?
			// stan.DeliverAllAvailable(),
			stan.StartAtTime(time.Unix(0, m.timeStamp)),
			stan.SetManualAckMode(),
			stan.AckWait(time.Second*10), // needed so ack mode is on ...
			stan.MaxInflight(10000),
		)
		if err != nil {
			clog(log.ErrorLevel, "failed to subscribe to data channel %s: %s",
				func() interface{} { return subDataName },
				func() interface{} { return err })
		}

	case "stop":
		clog(log.InfoLevel, "received stop message, unsubscribe to data channel")
		if subcData == nil {
			break
		}
		if err = subcData.Unsubscribe(); err != nil {
			clog(log.FatalLevel, "failed to unsubscribe to data channel %s: %s",
				func() interface{} { return subDataName },
				func() interface{} { return err })
		}

	default:
		clog(log.ErrorLevel, "unhandled message %s",
			func() interface{} { return m.Which })
	}
}

func onMsgControl(m *stan.Msg) {
	clog(log.InfoLevel, "received message at [%v] on [%v] seq [%v]: %s, redelivared: [%v]\n",
		func() interface{} { return time.Unix(0, m.Timestamp) },
		func() interface{} { return m.Subject },
		func() interface{} { return m.Sequence },
		func() interface{} { return string(m.Data) },
		func() interface{} { return m.Redelivered })

	clog(log.DebugLevel, "got control message with TS %v(%v)",
		func() interface{} { return time.Unix(0, m.Timestamp) },
		func() interface{} { return m.Timestamp })

	var val map[string]interface{}
	json.Unmarshal(m.Data, &val)
	clog(log.DebugLevel, "after unmarshaling: %#v\n",
		func() interface{} { return val })
	out, err := json.MarshalIndent(val, " ..x", " ..z")
	clog(log.DebugLevel, "after unmarshaling and marhsaling (err: %v): data:%v - toprint:%v\n",
		func() interface{} { return err },
		func() interface{} { return val },
		func() interface{} { return string(out) })

	v2 := NewMsg()
	_, ok := val["which"]
	if ok {
		v2.Which, ok = val["which"].(string)
		v2.timeStamp = m.Timestamp
		if ok {
			handleMsgControl(v2)
		}
	}
	if !ok {
		clog(log.ErrorLevel, "control message which element errornous : %#v",
			func() interface{} { return m })
	}

	m.Ack()
}

func main() {

	var err error
	var xtraArgs []string
	xtraArgs, err = flags.NewParser(&Opts, flags.Default).Parse()
	if err != nil {
		if e, ok := err.(*flags.Error); ok {
			switch e.Type {
			case flags.ErrHelp:
				os.Exit(0)
			case flags.ErrInvalidChoice:
				clog(log.ErrorLevel, "invalid choice")
			default:
				clog(log.ErrorLevel, "error parsing opts: %v\n",
					func() interface{} { return e.Type })
			}
		} else {
			clog(log.ErrorLevel, "unknown error: %v\n",
				func() interface{} { return err })
		}
		os.Exit(1)
		/*
			clog(log.FatalLevel, "argument parsing failed %s",
				func() interface{} { return err })
		*/
	}

	if Opts.LogLevel != "" {
		if logLevel, err := log.ParseLevel(Opts.LogLevel); err != nil {
			clog(log.FatalLevel, "invalid global log level %s - %s",
				func() interface{} { return Opts.LogLevel },
				func() interface{} { return err })
		} else {
			log.SetLevel(logLevel)
		}
	}

	clog(log.InfoLevel, "xtraArgs: %v",
		func() interface{} { return xtraArgs })

	if Opts.StanHost != "" {
		stanURL = fmt.Sprintf("nats://%s:4222", Opts.StanHost)
	}

	// MEM profiling
	if Opts.MEMProfile != "" {
		f, err := os.Create(Opts.MEMProfile)
		if err != nil {
			clog(log.FatalLevel, "profiling memory failed : %s",
				func() interface{} { return err })
		}
		clog(log.InfoLevel, "enable memprofiling, write to '%v'\n",
			func() interface{} { return Opts.MEMProfile })
		defer func() {
			pprof.WriteHeapProfile(f)
			f.Close()
			return
		}()
	}

	// CPU profiling
	if Opts.CPUProfile != "" {
		f, err := os.Create(Opts.CPUProfile)
		if err != nil {
			clog(log.FatalLevel, "profiling cpu failed : %s",
				func() interface{} { return err })
		}
		clog(log.InfoLevel, "enable cpuprofiling, write to '%v'\n",
			func() interface{} { return Opts.CPUProfile })
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}

	clog(log.InfoLevel, "connecting to %s (cluster id: %s, client id: %s)",
		func() interface{} { return stanURL },
		func() interface{} { return stanClusterID },
		func() interface{} { return stanClientID })

	sc, err = stan.Connect(
		stanClusterID,
		stanClientID,
		stan.NatsURL(stanURL),
		stan.SetConnectionLostHandler(func(conn stan.Conn, err error) {
			var opts nats.Options
			if natsConn := conn.NatsConn(); natsConn != nil {
				opts = natsConn.Opts
			}
			clog(log.ErrorLevel, "connection lost to %v: %s",
				func() interface{} { return opts },
				func() interface{} { return err },
			)
		}),
		stan.ConnectWait(time.Second*30),
	)

	if err != nil {
		clog(log.FatalLevel, "stan.Connect failed: %v",
			func() interface{} { return err })
	}

	if Opts.Manual != "" {
		waitDone := make(chan struct{})
		fmt.Printf("manual ... %v\n", Opts.Manual)
		prefixStr := strings.Split(Opts.Manual, "_")[0]
		nexthopStr := strings.Split(Opts.Manual, "_")[1]

		_, prefix, _ := net.ParseCIDR(prefixStr)
		nexthop := net.ParseIP(nexthopStr)

		fmt.Printf("%v:%v\n", prefixStr, nexthopStr)
		dataBuf := &bytes.Buffer{}
		msgEncoder := msgpack.NewEncoder(dataBuf)
		msgEncoder.UseJSONTag(true)
		var i2 Item2MsgPack
		i2.TagBegin = 0xdeadbeaf
		i2.TagEnd = 0xbeafdead
		i2.MED = 100
		clog(log.InfoLevel, "%T:%v\n",
			func() interface{} { return prefix },
			func() interface{} { return prefix })
		high, low := ipnumbers.NetIPtouint64(&prefix.IP)

		ones, _ := prefix.Mask.Size()
		i2.Prefix[0] = uint32(ones)
		i2.Prefix[1] = uint32(high >> 32)
		i2.Prefix[2] = uint32(high & 0xffffffff)
		i2.Prefix[3] = uint32(low >> 32)
		i2.Prefix[4] = uint32(low & 0xffffffff)

		high, low = ipnumbers.NetIPtouint64(&nexthop)
		i2.NextHop[0] = uint32(high >> 32)
		i2.NextHop[1] = uint32(high & 0xffffffff)
		i2.NextHop[2] = uint32(low >> 32)
		i2.NextHop[3] = uint32(low & 0xffffffff)

		if nexthop.Equal(net.ParseIP("0.0.0.0")) || nexthop.Equal(net.ParseIP("::0")) {
			// this is withdrawl
		}
		i2.Which = whichManual
		err = msgEncoder.Encode(&i2)
		if err != nil {
			clog(log.ErrorLevel, "msg pack encoding failed: %s",
				func() interface{} { return err })
		}
		// log.Printf("i2 to publish: %s\n%v\n%#q\n", i2, hex.Dump(dataBuf.Bytes()), i2)

		retStr, err := sc.PublishAsync("bar", dataBuf.Bytes(), func(guid string, err error) {
			if err != nil {
				clog(log.ErrorLevel, "publish manual - ack failed for message guid: %s, %s\n",
					func() interface{} { return guid },
					func() interface{} { return err })
			} else {
				clog(log.DebugLevel, "publish manual - got ack: %s, %v\n",
					func() interface{} { return guid },
					func() interface{} { return err })
				waitDone <- struct{}{}
			}
		})
		if err != nil {
			clog(log.ErrorLevel, "publish manual - failed for message %s (guid: %s), err %s",
				func() interface{} { return dataBuf },
				func() interface{} { return retStr },
				func() interface{} { return err })
		}
		clog(log.TraceLevel, "published: \n%v\n%v\n",
			func() interface{} { return i2 },
			func() interface{} { return hex.Dump(dataBuf.Bytes()) })

		<-waitDone
	} else {
		subcControl, err = sc.Subscribe(
			subControlName,
			onMsgControl,
			// stan.StartAtTimeDelta(time.Second*60),
			// TODO: maybe this should correlate to bgp gracefull time ?
			stan.DeliverAllAvailable(),
			stan.SetManualAckMode(),
			stan.AckWait(time.Second*10), // needed so ack mode is on ...
		)
		if err != nil {
			clog(log.FatalLevel, "unable to subscribe to %s: %v",
				func() interface{} { return subControlName },
				func() interface{} { return err })
		}
		time.Sleep(time.Second * 60)
	}
}
