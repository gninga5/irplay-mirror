// Genera uno stream HEVC con VideoToolbox (come fa iOS) per testare la parte HEVC del motore.
// Uso: gen_hevc <prefisso> <larghezza> <altezza> <riordino 0|1>
// Output: <prefisso>.vps/.sps/.pps (NAL senza start code) e <prefisso>.frames
//         ([u32 LE lunghezza][u8 keyframe][NAL con prefisso di lunghezza a 4 byte...]*)
import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo

let args = CommandLine.arguments
guard args.count == 5, let width = Int(args[2]), let height = Int(args[3]) else {
    fatalError("uso: gen_hevc <prefisso> <larghezza> <altezza> <riordino 0|1>")
}
let prefix = args[1], reorder = args[4] == "1", count = 120
var frames = Data()
var params: [Data]? = nil

func parameterSets(_ fd: CMFormatDescription) -> [Data] {
    var n = 0
    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                       parameterSetSizeOut: nil, parameterSetCountOut: &n, nalUnitHeaderLengthOut: nil)
    return (0..<n).map { i in
        var p: UnsafePointer<UInt8>? = nil, len = 0
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p,
                                                           parameterSetSizeOut: &len, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
        return Data(bytes: p!, count: len)
    }
}

var session: VTCompressionSession? = nil
let st = VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_HEVC,
                                    encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                                    outputCallback: nil, refcon: nil, compressionSessionOut: &session)
guard st == noErr, let s = session else { fatalError("encoder HEVC non disponibile (\(st))") }
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_HEVC_Main_AutoLevel)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: reorder ? kCFBooleanTrue : kCFBooleanFalse)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 60 as CFNumber)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: 6_000_000 as CFNumber)

for i in 0..<count {
    var pb: CVPixelBuffer? = nil
    CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_32BGRA, nil, &pb)
    let buf = pb!
    CVPixelBufferLockBaseAddress(buf, [])
    let base = CVPixelBufferGetBaseAddress(buf)!.assumingMemoryBound(to: UInt8.self)
    let stride = CVPixelBufferGetBytesPerRow(buf)
    for y in 0..<height {
        for x in 0..<width {
            let p = base + y * stride + x * 4
            let bar = ((x + i * 12) / max(1, width / 8)) % 8
            p[0] = UInt8((bar * 37 + y / 4) & 0xFF); p[1] = UInt8((bar * 91 + i * 3) & 0xFF); p[2] = UInt8((x / 4 + i) & 0xFF); p[3] = 255
        }
    }
    CVPixelBufferUnlockBaseAddress(buf, [])
    let pts = CMTime(value: CMTimeValue(i), timescale: 60)
    VTCompressionSessionEncodeFrame(s, imageBuffer: buf, presentationTimeStamp: pts, duration: .invalid, frameProperties: nil,
                                    infoFlagsOut: nil) { status, _, sb in
        guard status == noErr, let sb = sb, let bb = CMSampleBufferGetDataBuffer(sb) else { return }
        if params == nil, let fd = CMSampleBufferGetFormatDescription(sb) { params = parameterSets(fd) }
        var key = true
        if let att = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]],
           let nd = att.first?[kCMSampleAttachmentKey_NotSync] as? Bool { key = !nd }
        var len = 0; var ptr: UnsafeMutablePointer<Int8>? = nil
        CMBlockBufferGetDataPointer(bb, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr)
        var l = UInt32(len).littleEndian
        frames.append(Data(bytes: &l, count: 4)); frames.append(key ? 1 : 0)
        frames.append(Data(bytes: ptr!, count: len))
    }
}
VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
guard let ps = params, ps.count >= 3 else { fatalError("parametri HEVC assenti") }
try! ps[0].write(to: URL(fileURLWithPath: prefix + ".vps"))
try! ps[1].write(to: URL(fileURLWithPath: prefix + ".sps"))
try! ps[2].write(to: URL(fileURLWithPath: prefix + ".pps"))
try! frames.write(to: URL(fileURLWithPath: prefix + ".frames"))
print("\(prefix): \(count) frame HEVC \(width)x\(height), riordino \(reorder ? "sì" : "no"), VPS \(ps[0].count) SPS \(ps[1].count) PPS \(ps[2].count) byte")
