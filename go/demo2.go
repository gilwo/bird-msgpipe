package main

import (
	"bytes"
	"fmt"
	"github.com/gilwo/ipnumbers"
	log "github.com/sirupsen/logrus"
	"github.com/vmihailenco/msgpack"
	"net"
)

type WhichType uint8

const (
	whichUnkown WhichType = iota
	whichAdd    WhichType = iota
	whichDel    WhichType = iota
	whichRem    WhichType = iota
	whichManual WhichType = iota
)

func (w WhichType) String() string {
	switch w {
	case whichAdd:
		return "update"
	case whichDel:
		return "withdraw"
	case whichRem:
		return "withdraw (remove)"
	case whichManual:
		return "manual"
	}
	return fmt.Sprintf("unknown: %X", uint8(w))
}

type Item2 struct {
	Prefix         [5]uint32   `json:"p,omitempty"`
	Best           [4]uint32   `json:"b,omitempty"`
	Which          WhichType   `json:"w,omitempty"`
	Origin         uint32      `json:"1,omitempty"`
	AsPath         [][]uint32  `json:"2,omitempty"`
	NextHop        [4]uint32   `json:"3,omitempty"`
	MED            uint32      `json:"4,omitempty"`
	LocalPref      uint32      `json:"5,omitempty"`
	AtomicAggr     []uint32    `json:"6,omitempty"`
	Aggr           []uint32    `json:"7,omitempty"`
	Community      [][2]uint32 `json:"8,omitempty"`
	OriginatorID   uint32      `json:"9,omitempty"`
	ClusterList    []uint32    `json:"10,omitempty"`
	MPReachNlri    uint32      `json:"14,omitempty"`
	MPUnReachNlri  uint32      `json:"15,omitempty"`
	ExtCommunity   []uint64    `json:"16,omitempty"`
	As4Path        [][]uint32  `json:"17,omitempty"`
	As4Aggr        uint32      `json:"18,omitempty"`
	LargeCommunity [][3]uint32 `json:"32,omitempty"`
}

type Item2MsgPack struct {
	TagBegin uint32 `json:"s,omitempty"`
	Item2
	TagEnd uint32 `json:"e,omitempty"`
}

func (i *Item2) getPrefix() *net.IPNet {
	high := uint64(i.Prefix[1])<<32 | uint64(i.Prefix[2])
	low := uint64(i.Prefix[3])<<32 | uint64(i.Prefix[4])
	_, netip := ipnumbers.Uint64toip(high, low)

	cidrbits := 32
	if netip.To4() == nil {
		cidrbits = 128
	}
	return &net.IPNet{IP: netip, Mask: net.CIDRMask(int(i.Prefix[0]), cidrbits)}
}

func (i *Item2) getNextHop() *net.IP {
	high := uint64(i.NextHop[0])<<32 | uint64(i.NextHop[1])
	low := uint64(i.NextHop[2])<<32 | uint64(i.NextHop[3])
	_, netip := ipnumbers.Uint64toip(high, low)

	return &netip
}
func (i *Item2) getBest() *net.IP {
	high := uint64(i.Best[0])<<32 | uint64(i.Best[1])
	low := uint64(i.Best[2])<<32 | uint64(i.Best[3])
	_, netip := ipnumbers.Uint64toip(high, low)

	return &netip
}

func (i *Item2) getASPath() string {
	out := ""
	for _, e := range i.AsPath {
		asPathType := 0
		for i, e2 := range e {
			if i == 0 {
				asPathType = int(e2)
				switch asPathType {
				case 1:
					out += fmt.Sprintf(" (set) { ")
				case 2:
					out += fmt.Sprintf(" (seq) ")
				case 3:
					out += fmt.Sprintf(" (confed seq) ( ")
				case 4:
					out += fmt.Sprintf(" (confed set) ({ ")
				}
				continue
			}
			out += fmt.Sprintf("%v ", e2)
		}
		switch asPathType {
		case 1:
			out += fmt.Sprintf("}, ")
		case 2:
			out += fmt.Sprintf(", ")
		case 3:
			out += fmt.Sprintf("), ")
		case 4:
			out += fmt.Sprintf("}), ")
		}
	}
	// return out[:len(out)-1]
	return out
}

func (i Item2) String() string {
	return fmt.Sprintf(`
%s - %v
	as_path: %v
	next_hop: %s
	med: %d
	local_pref: %d
	community: %v
	large_communitiy: %v
	best %s`,
		i.getPrefix(),
		i.Which,
		i.getASPath(),
		// i.AsPath[0][1:],
		i.getNextHop(),
		i.MED,
		i.LocalPref,
		i.Community,
		i.LargeCommunity,
		i.getBest())
}

func decodeMsg(data []byte) *Item2MsgPack {
	var err error
	msgDecoder := msgpack.NewDecoder(bytes.NewBuffer(data))
	msgDecoder.UseJSONTag(true)
	var i2 Item2MsgPack
	err = msgDecoder.Decode(&i2)
	if err != nil {
		clog(log.ErrorLevel, "failed to msgpack decode on decoder: %s",
			func() interface{} { return err })
		// log.Errorf("%s\n", hex.Dump(m.Data))
	} else {
		clog(log.TraceLevel, "!!! success i2 %v\n",
			func() interface{} { return i2 })

		high := uint64(i2.Prefix[1])<<32 | uint64(i2.Prefix[2])
		low := uint64(i2.Prefix[3])<<32 | uint64(i2.Prefix[4])
		_, netip := ipnumbers.Uint64toip(high, low)
		// log.Printf("prefix : %v/%d", netip, i2.Prefix[0])
		cidrbits := 32
		if netip.To4() == nil {
			cidrbits = 128
		}
		prefix := net.IPNet{IP: netip, Mask: net.CIDRMask(int(i2.Prefix[0]), cidrbits)}
		clog(log.TraceLevel, "prefix : %v",
			func() interface{} { return &prefix })
		clog(log.TraceLevel, "item: %s",
			func() interface{} { return i2 })
		// log.Trace("\n%s\n", hex.Dump(m.Data))
	}
	clog(log.DebugLevel, "route information recieved for: %s\n",
		func() interface{} { return i2 })
	return &i2
}
