// Decodifica con VideoToolbox lo stesso flusso HEVC con i parametri originali e con quelli
// riscritti (<prefisso>.p.vps/.p.sps) e confronta le immagini pixel per pixel.
// Uso: vt_hevc_check <prefisso>
import Foundation
import VideoToolbox
import CoreMedia

let prefix = CommandLine.arguments[1]
func load(_ ext: String) -> Data { try! Data(contentsOf: URL(fileURLWithPath: prefix + "." + ext)) }
let frames = load("frames")

func decode(vps: Data, sps: Data, pps: Data) -> [Data] {
    var fd: CMFormatDescription? = nil
    let sets = [vps, sps, pps]
    let ptrs = sets.map { d -> UnsafeMutablePointer<UInt8> in
        let p = UnsafeMutablePointer<UInt8>.allocate(capacity: d.count); d.copyBytes(to: p, count: d.count); return p }
    let st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(allocator: nil, parameterSetCount: 3,
        parameterSetPointers: ptrs.map { UnsafePointer($0) }, parameterSetSizes: sets.map { $0.count },
        nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &fd)
    precondition(st == noErr, "formato HEVC rifiutato: \(st)")
    var out: [Data] = []
    var session: VTDecompressionSession? = nil
    let attrs = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange] as CFDictionary
    VTDecompressionSessionCreate(allocator: nil, formatDescription: fd!, decoderSpecification: nil,
                                 imageBufferAttributes: attrs, outputCallback: nil, decompressionSessionOut: &session)
    var i = 0
    while i + 5 <= frames.count {
        let len = Int(frames[i..<i+4].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) })
        let payload = frames.subdata(in: (i + 5)..<(i + 5 + len))
        i += 5 + len
        var bb: CMBlockBuffer? = nil
        let mem = UnsafeMutableRawPointer.allocate(byteCount: payload.count, alignment: 1)
        payload.copyBytes(to: mem.assumingMemoryBound(to: UInt8.self), count: payload.count)
        CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: mem, blockLength: payload.count, blockAllocator: kCFAllocatorDefault,
                                           customBlockSource: nil, offsetToData: 0, dataLength: payload.count, flags: 0, blockBufferOut: &bb)
        var sb: CMSampleBuffer? = nil
        var size = payload.count
        CMSampleBufferCreateReady(allocator: nil, dataBuffer: bb, formatDescription: fd, sampleCount: 1, sampleTimingEntryCount: 0,
                                  sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sb)
        VTDecompressionSessionDecodeFrame(session!, sampleBuffer: sb!, flags: [], infoFlagsOut: nil) { status, _, img, _, _ in
            guard status == noErr, let img = img else { return }
            CVPixelBufferLockBaseAddress(img, .readOnly)
            var d = Data()
            for plane in 0..<CVPixelBufferGetPlaneCount(img) {
                let base = CVPixelBufferGetBaseAddressOfPlane(img, plane)!
                d.append(Data(bytes: base, count: CVPixelBufferGetBytesPerRowOfPlane(img, plane) * CVPixelBufferGetHeightOfPlane(img, plane)))
            }
            CVPixelBufferUnlockBaseAddress(img, .readOnly)
            out.append(d)
        }
    }
    VTDecompressionSessionWaitForAsynchronousFrames(session!)
    return out
}

let a = decode(vps: load("vps"), sps: load("sps"), pps: load("pps"))
let b = decode(vps: load("p.vps"), sps: load("p.sps"), pps: load("pps"))
let diff = zip(a, b).filter { $0.0 != $0.1 }.count
print("\(prefix): \(b.count) immagini decodificate con i parametri riscritti (\(a.count) con gli originali), differenze: \(diff)")
if a.count != b.count || diff != 0 { exit(1) }
