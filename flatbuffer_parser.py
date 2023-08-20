import sys
sys.path.append("/root/flatbuffers/python/")
import Bela.Watcher.DataMsg as DataMsg
import Bela.Watcher.FileHeader as FileHeader
import flatbuffers
import array

filename = 'myvar3.bin'
buf = open(filename, 'rb').read()
buf = bytearray(buf)
offset = 0
fileHeaderSize = flatbuffers.util.GetSizePrefix(buf, offset)
(buf, offset) = flatbuffers.util.RemoveSizePrefix(buf, offset)
fileHeader = FileHeader.FileHeader.GetRootAs(buf, offset)
print(f"File {filename}: {fileHeader.What()}, var_name: {fileHeader.VarName()}, pid: {fileHeader.Pid()}, ptr: {fileHeader.Ptr():#010x}")
offset += fileHeaderSize
# for each buffer
oldTimestamp = 0;
while offset < len(buf):
#if 1:
    msgSize = flatbuffers.util.GetSizePrefix(buf, offset)
    (buf, offset) = flatbuffers.util.RemoveSizePrefix(buf, offset)
    msg = DataMsg.DataMsg.GetRootAs(buf, offset)
    timestamp = msg.Timestamp()
    typeId = msg.TypeId().decode("utf-8")
    if("j" == typeId) :
        typeId = "I"
        # TODO: add more conversions here
    timestampType = "SPARSE" if msg.PayloadTimestampSize() else "DENSE"
    print(f"{timestampType} offset: {offset} size: {msgSize}, timestamp: {timestamp}({timestamp-oldTimestamp}) varId: {msg.VarId()}, type_id: {typeId}({msg.TypeId()}), payloadSize: {msg.PayloadDataSize()}, payloadTimestampSize: {msg.PayloadTimestampSize()}")
    oldTimestamp = msg.Timestamp()
    offset += msgSize
    data = array.array(typeId)
    data.frombytes(buf[offset : offset + msg.PayloadDataSize()])
    if(1): # print elements
        for n in range(len(data)):
            print(f"{data[n]} ", end="")
        print()
    offset += msg.PayloadDataSize()
    timestamps = array.array("I") # timestamps are uint32_t
    timestamps.frombytes(buf[offset : offset + msg.PayloadTimestampSize()])
    if(1): # print timestamps
        for n in range(len(timestamps)):
            print(f"{timestamps[n]} ", end="")
        if(len(timestamps)):
            print()
    offset += msg.PayloadTimestampSize()

