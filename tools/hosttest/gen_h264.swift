// Genera uno stream H.264 AVCC con VideoToolbox (come fa iOS) per testare la pipeline video.
// Output: avcc.bin (record avcC) e frames.bin ([u32 LE len][u8 key][AVCC...]*)
import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo

let width = 1280, height = 720, count = 180
var frames = Data()
var avcc: Data? = nil

func makeAvcC(_ fd: CMFormatDescription) -> Data {
    var spsPtr: UnsafePointer<UInt8>? = nil, spsLen = 0, ppsPtr: UnsafePointer<UInt8>? = nil, ppsLen = 0, n = 0, hl: Int32 = 0
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: &spsPtr, parameterSetSizeOut: &spsLen, parameterSetCountOut: &n, nalUnitHeaderLengthOut: &hl)
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: 1, parameterSetPointerOut: &ppsPtr, parameterSetSizeOut: &ppsLen, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
    let sps = Data(bytes: spsPtr!, count: spsLen), pps = Data(bytes: ppsPtr!, count: ppsLen)
    var d = Data([1, sps[1], sps[2], sps[3], 0xFF, 0xE1, UInt8(spsLen >> 8), UInt8(spsLen & 0xFF)])
    d.append(sps); d.append(contentsOf: [1, UInt8(ppsLen >> 8), UInt8(ppsLen & 0xFF)]); d.append(pps)
    return d
}

var session: VTCompressionSession? = nil
VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264,
                           encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                           outputCallback: nil, refcon: nil, compressionSessionOut: &session)
guard let s = session else { fatalError("encoder") }
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 60 as CFNumber)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: 4_000_000 as CFNumber)

for i in 0..<count {
    var pb: CVPixelBuffer? = nil
    CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_32BGRA, nil, &pb)
    let buf = pb!
    CVPixelBufferLockBaseAddress(buf, [])
    let base = CVPixelBufferGetBaseAddress(buf)!.assumingMemoryBound(to: UInt8.self)
    let stride = CVPixelBufferGetBytesPerRow(buf)
    let colors: [(UInt8, UInt8, UInt8)] = [(255,255,255),(0,255,255),(255,255,0),(0,255,0),(255,0,255),(0,0,255),(255,0,0),(0,0,0)]
    for y in 0..<height {
        for x in 0..<width {
            let bar = ((x + i * 8) / (width / 8)) % 8
            var (b, g, r) = colors[bar]
            if y > height * 3 / 4 { (b, g, r) = (UInt8((x + i * 4) & 0xFF), UInt8(y & 0xFF), 128) }
            let p = base + y * stride + x * 4
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255
        }
    }
    CVPixelBufferUnlockBaseAddress(buf, [])
    let pts = CMTime(value: CMTimeValue(i), timescale: 30)
    VTCompressionSessionEncodeFrame(s, imageBuffer: buf, presentationTimeStamp: pts, duration: .invalid, frameProperties: nil, infoFlagsOut: nil) { status, _, sb in
        guard status == noErr, let sb = sb, let bb = CMSampleBufferGetDataBuffer(sb) else { return }
        if avcc == nil, let fd = CMSampleBufferGetFormatDescription(sb) { avcc = makeAvcC(fd) }
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
try! avcc!.write(to: URL(fileURLWithPath: "avcc.bin"))
try! frames.write(to: URL(fileURLWithPath: "frames.bin"))
print("avcC \(avcc!.count) byte, frames \(frames.count) byte")
