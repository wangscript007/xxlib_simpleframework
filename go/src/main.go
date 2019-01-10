package main

import (
	"fmt"
	"unsafe"
)

type NullableUInt8 struct {
	Value uint8
	HasValue bool
}
type NullableUInt16 struct {
	Value uint16
	HasValue bool
}
type NullableUInt32 struct {
	Value uint32
	HasValue bool
}
type NullableUInt64 struct {
	Value uint64
	HasValue bool
}
type NullableInt8 struct {
	Value int8
	HasValue bool
}
type NullableInt16 struct {
	Value int16
	HasValue bool
}
type NullableInt32 struct {
	Value int32
	HasValue bool
}
type NullableInt64 struct {
	Value int64
	HasValue bool
}
type NullableBool struct {
	Value bool
	HasValue bool
}
type NullableFloat32 struct {
	Value float32
	HasValue bool
}
type NullableFloat64 struct {
	Value float64
	HasValue bool
}
type NullableString struct {
	Value string
	HasValue bool
}




type IObject interface {
	GetPackageId() uint16
	ToBBuffer(bb *BBuffer)
	FromBBuffer(bb *BBuffer)
}





type BBuffer struct {
	Buf []uint8
	Offset int
	ReadLengthLimit int
	// todo
}
func (zs *BBuffer) DataLen() int {
	return len(zs.Buf)
}
func (zs *BBuffer) Clear() {
	zs.Buf = zs.Buf[:0]
	zs.Offset = 0
	zs.ReadLengthLimit = 0
	// todo
}

func (zs *BBuffer) GetPackageId() uint16 {
	return 2
}
func (zs *BBuffer) ToBBuffer(bb *BBuffer) {
	if zs == nil {
		bb.Buf = append(bb.Buf, uint8(0))
		return
	}
	bb.Buf = append(bb.Buf, uint8(2))		// typeId
	bb.WriteLength(len(zs.Buf))
	bb.Buf = append(bb.Buf, zs.Buf...)
}
func (zs *BBuffer) FromBBuffer(bb *BBuffer) {
	bufLen := bb.ReadLength()
	if bb.ReadLengthLimit != 0 && bufLen > bb.ReadLengthLimit {
		panic(-1)
	}
	zs.Clear()
	zs.Buf = append(zs.Buf, bb.Buf[bb.Offset:bb.Offset + bufLen]...)
	bb.Offset += bufLen
}

// for all fix uint
func (zs *BBuffer) WriteVarUInt64(v uint64) {
	for v >= 1<<7 {
		zs.Buf = append(zs.Buf, uint8(v & 0x7f | 0x80))
		v >>= 7
	}
	zs.Buf = append(zs.Buf, uint8(v))
}
func (zs *BBuffer) ReadVarUInt64() (r uint64) {
	offset := zs.Offset
	for shift := uint(0); shift < 64; shift += 7 {
		b := uint64(zs.Buf[offset])
		offset++
		r |= (b & 0x7F) << shift
		if (b & 0x80) == 0 {
			zs.Offset = offset
			return r
		}
	}
	panic(-1)
}


// for all fix int
func (zs *BBuffer) WriteVarInt64(v int64) {
	zs.WriteVarUInt64( uint64(uint64(v << 1) ^ uint64(v >> 63)) )
}
func (zs *BBuffer) ReadVarInt64() int64 {
	v := zs.ReadVarUInt64()
	return (int64)(v >> 1) ^ (-(int64)(v & 1))
}

// for all len
func (zs *BBuffer) WriteLength(v int) {
	zs.WriteVarUInt64( uint64(v) )
}
func (zs *BBuffer) ReadLength() int {
	return int(zs.ReadVarUInt64())
}



func (zs *BBuffer) WriteFloat32(v float32) {
	f := *(*uint32)(unsafe.Pointer(&v))
	zs.Buf = append(zs.Buf,
		uint8(f),
		uint8(f >> 8),
		uint8(f >> 16),
		uint8(f >> 24))
}
func (zs *BBuffer) ReadFloat32() (r float32) {
	v := uint32(zs.Buf[zs.Offset]) |
		uint32(zs.Buf[zs.Offset + 1]) << 8 |
		uint32(zs.Buf[zs.Offset + 2]) << 16 |
		uint32(zs.Buf[zs.Offset + 3]) << 24
	r = *(*float32)(unsafe.Pointer(&v))
	zs.Offset += 4
	return
}

func (zs *BBuffer) WriteFloat64(v float64) {
	d := *(*uint64)(unsafe.Pointer(&v))
	zs.Buf = append(zs.Buf,
		uint8(d),
		uint8(d >> 8),
		uint8(d >> 16),
		uint8(d >> 24),
		uint8(d >> 32),
		uint8(d >> 40),
		uint8(d >> 48),
		uint8(d >> 56))
}
func (zs *BBuffer) ReadFloat64() (r float64) {
	v := uint32(zs.Buf[zs.Offset]) |
		uint32(zs.Buf[zs.Offset + 1]) << 8 |
		uint32(zs.Buf[zs.Offset + 2]) << 16 |
		uint32(zs.Buf[zs.Offset + 3]) << 24 |
		uint32(zs.Buf[zs.Offset + 4]) << 32 |
		uint32(zs.Buf[zs.Offset + 5]) << 40 |
		uint32(zs.Buf[zs.Offset + 6]) << 48 |
		uint32(zs.Buf[zs.Offset + 7]) << 56
	r = *(*float64)(unsafe.Pointer(&v))
	zs.Offset += 8
	return
}

func (zs *BBuffer) WriteBool(v bool) {
	if v {
		zs.Buf = append(zs.Buf, uint8(1))
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadBool() (r bool) {
	r = zs.Buf[zs.Offset] > 0
	zs.Offset++
	return
}

func (zs *BBuffer) WriteInt8(v int8) {
	zs.Buf = append(zs.Buf, uint8(v))
}
func (zs *BBuffer) ReadInt8() (r int8) {
	r = int8(zs.Buf[zs.Offset])
	zs.Offset++
	return
}

func (zs *BBuffer) WriteInt16(v int16) {
	zs.WriteVarInt64(int64(v))
}
func (zs *BBuffer) ReadInt16() int16 {
	return int16(zs.ReadVarInt64())
}

func (zs *BBuffer) WriteInt32(v int32) {
	zs.WriteVarInt64(int64(v))
}
func (zs *BBuffer) ReadInt32() int32 {
	return int32(zs.ReadVarInt64())
}

func (zs *BBuffer) WriteInt64(v int64) {
	zs.WriteVarInt64(v)
}
func (zs *BBuffer) ReadInt64() int64 {
	return zs.ReadVarInt64()
}

func (zs *BBuffer) WriteUInt8(v uint8) {
	zs.Buf = append(zs.Buf, v)
}
func (zs *BBuffer) ReadUInt8() (r uint8) {
	r = zs.Buf[zs.Offset]
	zs.Offset++
	return
}

func (zs *BBuffer) WriteUInt16(v uint16) {
	zs.WriteVarUInt64(uint64(v))
}
func (zs *BBuffer) ReadUInt16() uint16 {
	return uint16(zs.ReadVarUInt64())
}

func (zs *BBuffer) WriteUInt32(v uint32) {
	zs.WriteVarUInt64(uint64(v))
}
func (zs *BBuffer) ReadUInt32() uint32 {
	return uint32(zs.ReadVarUInt64())
}

func (zs *BBuffer) WriteUInt64(v uint64) {
	zs.WriteVarUInt64(v)
}
func (zs *BBuffer) ReadUInt64() uint64 {
	return zs.ReadVarUInt64()
}

func (zs *BBuffer) WriteNullableBool(v NullableBool) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteBool(v.Value)
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableBool() (r NullableBool) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadBool()
	}
	return
}
func (zs *BBuffer) WriteNullableFloat32(v NullableFloat32) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteFloat32(v.Value)
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableFloat32() (r NullableFloat32) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadFloat32()
	}
	return
}

func (zs *BBuffer) WriteNullableFloat64(v NullableFloat64) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteFloat64(v.Value)
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableFloat64() (r NullableFloat64) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadFloat64()
	}
	return
}

func (zs *BBuffer) WriteNullableString(v NullableString) {
	if v.HasValue {
		zs.WriteUInt16(1)	// typeId
		zs.WriteLength(len(v.Value))
		zs.Buf = append(zs.Buf, v.Value...)
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableString() (r NullableString) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		bufLen := zs.ReadLength()
		r.Value = string(zs.Buf[zs.Offset:zs.Offset + bufLen])
		zs.Offset += bufLen
	}
	return
}

func (zs *BBuffer) WriteNullableUInt8(v NullableUInt8) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1), v.Value)
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableUInt8() (r NullableUInt8) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadUInt8()
	}
	return
}

func (zs *BBuffer) WriteNullableUInt16(v NullableUInt16) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteVarUInt64(uint64(v.Value))
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableUInt16() (r NullableUInt16) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadUInt16()
	}
	return
}

func (zs *BBuffer) WriteNullableUInt32(v NullableUInt32) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteVarUInt64(uint64(v.Value))
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableUInt32() (r NullableUInt32) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadUInt32()
	}
	return
}

func (zs *BBuffer) WriteNullableUInt64(v NullableUInt64) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteVarUInt64(v.Value)
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableUInt64() (r NullableUInt64) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadUInt64()
	}
	return
}

func (zs *BBuffer) WriteNullableInt8(v NullableInt8) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1), uint8(v.Value))
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableInt8() (r NullableInt8) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadInt8()
	}
	return
}

func (zs *BBuffer) WriteNullableInt16(v NullableInt16) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteVarInt64(int64(v.Value))
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableInt16() (r NullableInt16) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadInt16()
	}
	return
}

func (zs *BBuffer) WriteNullableInt32(v NullableInt32) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteVarInt64(int64(v.Value))
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableInt32() (r NullableInt32) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadInt32()
	}
	return
}

func (zs *BBuffer) WriteNullableInt64(v NullableInt64) {
	if v.HasValue {
		zs.Buf = append(zs.Buf, uint8(1))
		zs.WriteVarInt64(v.Value)
	} else {
		zs.Buf = append(zs.Buf, uint8(0))
	}
}
func (zs *BBuffer) ReadNullableInt64() (r NullableInt64) {
	r.HasValue = zs.ReadBool()
	if r.HasValue {
		r.Value = zs.ReadInt64()
	}
	return
}




















































// 模拟生成物

type PKG_Foo struct {
	Id int32
	Name NullableString
	Age NullableInt32
}
func (zs *PKG_Foo) GetPackageId() uint16 {
	return uint16(3)
}
func (zs *PKG_Foo) ToBBuffer(bb *BBuffer) {
	if zs == nil {
		bb.Buf = append(bb.Buf, uint8(0))
		return
	}
	bb.Buf = append(bb.Buf, uint8(1))
	bb.WriteInt32(zs.Id)
	bb.WriteNullableString(zs.Name)
	bb.WriteNullableInt32(zs.Age)
}
func (zs *PKG_Foo) FromBBuffer(bb *BBuffer) {
	zs.Id = bb.ReadInt32()
	zs.Name = bb.ReadNullableString()
	zs.Age = bb.ReadNullableInt32()
}


type PKG_Foos struct {
	Foos *List_PKG_Foo
}
func (zs *PKG_Foos) GetPackageId() uint16 {
	return uint16(4)
}
func (zs *PKG_Foos) ToBBuffer(bb *BBuffer) {
	if zs == nil {
		bb.Buf = append(bb.Buf, uint8(0))
		return
	}
	zs.Foos.ToBBuffer(bb)
}
func (zs *PKG_Foos) FromBBuffer(bb *BBuffer) {
	if bb.ReadBool() {
		zs.Foos = &List_PKG_Foo{}
		zs.Foos.FromBBuffer(bb)
	}
}
type PKG_FooEx struct {
	PKG_Foo
	Weight int32
}
func (zs *PKG_FooEx) GetPackageId() uint16 {
	return uint16(5)
}
func (zs *PKG_FooEx) ToBBuffer(bb *BBuffer) {
	if zs == nil {
		bb.Buf = append(bb.Buf, uint8(0))
		return
	}
	zs.PKG_Foo.ToBBuffer(bb)
	bb.WriteInt32(zs.Weight)
}
func (zs *PKG_FooEx) FromBBuffer(bb *BBuffer) {
	zs.PKG_Foo.FromBBuffer(bb)
	zs.Weight = bb.ReadInt32()
}





type List_PKG_Foo []IObject
func (zs *List_PKG_Foo) GetPackageId() uint16 {
	return uint16(6)
}
func (zs *List_PKG_Foo) SwapRemoveAt(idx int) {
	count := len(*zs)
	if idx + 1 < count {
		(*zs)[idx] = (*zs)[count - 1]
	}
	*zs = (*zs)[:count - 1]
}
func (zs *List_PKG_Foo) ToBBuffer(bb *BBuffer) {
	if zs == nil {
		bb.Buf = append(bb.Buf, uint8(0))
		return
	}
	bb.Buf = append(bb.Buf, uint8(1))
	bb.WriteLength(len(*zs))
	for _, v := range *zs {
		v.ToBBuffer(bb)
	}
}
func (zs *List_PKG_Foo) FromBBuffer(bb *BBuffer) {
	// todo
}
func (zs *List_PKG_Foo) Add(v *PKG_Foo) {
	*zs = append(*zs, v)
}
func (zs *List_PKG_Foo) Add_PKG_FooEx(v *PKG_FooEx) {
	*zs = append(*zs, v)
}
func (zs *List_PKG_Foo) At(idx int) *PKG_Foo {
	return (*zs)[idx].(*PKG_Foo)
}
func (zs *List_PKG_Foo) At_PKG_FooEx(idx int) *PKG_FooEx {
	return (*zs)[idx].(*PKG_FooEx)
}



func main() {
	bb := BBuffer{}
	foo := PKG_Foo{ 10, NullableString{"asdf", true}, NullableInt32{20, true } }
	fooex := PKG_FooEx {foo, 30 }
	foos := PKG_Foos { &List_PKG_Foo{} }
	foos.Foos.Add_PKG_FooEx( &fooex )
	foos.Foos.Add( nil )
	foos.Foos.Add( nil )
	foos.Foos.Add( nil )
	foos.Foos.Add( &foo )
	foos.ToBBuffer(&bb)
	fmt.Println(bb)
	bb.Clear()

	dump := func() {
		for _, f := range *foos.Foos {
			switch f.(type) {
			case *PKG_Foo:
				o := f.(*PKG_Foo)
				if o == nil {
					fmt.Println("PKG_Foo(nil)")
				} else {
					fmt.Println("PKG_Foo")
				}
			case *PKG_FooEx:
				o := f.(*PKG_FooEx)
				if o == nil {
					fmt.Println("PKG_FooEx(nil)")
				} else {
					fmt.Println("PKG_FooEx")
				}
			case nil:
				fmt.Println("nil")
			}
		}
		fmt.Println()
	}
	dump()
	foos.Foos.SwapRemoveAt(1)
	dump()
}
