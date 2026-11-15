import Foundation
import VideoToolbox
import CoreMedia
let sps = try! Data(contentsOf: URL(fileURLWithPath: "sps_patched.bin")), pps = try! Data(contentsOf: URL(fileURLWithPath: "pps.bin"))
var fd: CMFormatDescription?
let st = sps.withUnsafeBytes { s in pps.withUnsafeBytes { p -> OSStatus in
    var ptrs = [s.bindMemory(to: UInt8.self).baseAddress!, p.bindMemory(to: UInt8.self).baseAddress!]
    var sizes = [sps.count, pps.count]
    return CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: nil, parameterSetCount: 2, parameterSetPointers: &ptrs, parameterSetSizes: &sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &fd) } }
print("format description:", st == noErr ? "OK" : "ERRORE \(st)")
var session: VTDecompressionSession?
var ok = 0, bad = 0
let s2 = VTDecompressionSessionCreate(allocator: nil, formatDescription: fd!, decoderSpecification: nil, imageBufferAttributes: nil, outputCallback: nil, decompressionSessionOut: &session)
print("sessione:", s2 == noErr ? "OK" : "ERRORE \(s2)")
let frames = try! Data(contentsOf: URL(fileURLWithPath: "frames.bin"))
var p = 0
while p + 5 <= frames.count {
    let n = Int(frames[p]) | Int(frames[p+1]) << 8 | Int(frames[p+2]) << 16 | Int(frames[p+3]) << 24
    var data = frames.subdata(in: (p+5)..<(p+5+n)); p += 5 + n
    data.withUnsafeMutableBytes { raw in
        var bb: CMBlockBuffer?; var sb: CMSampleBuffer?
        CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: raw.baseAddress, blockLength: n, blockAllocator: kCFAllocatorNull, customBlockSource: nil, offsetToData: 0, dataLength: n, flags: 0, blockBufferOut: &bb)
        var sz = n
        CMSampleBufferCreateReady(allocator: nil, dataBuffer: bb, formatDescription: fd, sampleCount: 1, sampleTimingEntryCount: 0, sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &sz, sampleBufferOut: &sb)
        let r = VTDecompressionSessionDecodeFrame(session!, sampleBuffer: sb!, flags: [], infoFlagsOut: nil) { status, _, img, _, _ in
            if status == noErr && img != nil { ok += 1 } else { bad += 1 } }
        if r != noErr { bad += 1 }
    }
}
VTDecompressionSessionWaitForAsynchronousFrames(session!)
print("frame decodificati con SPS riscritto: \(ok), errori: \(bad)")
